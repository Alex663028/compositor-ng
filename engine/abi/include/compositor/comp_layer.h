// AI生成
/*
 * comp_layer.h — 图层、调整图层与图层效果。
 *
 * 图层树约定：
 *   - 顺序自下而上（数组序即堆叠序），与 .comp 规范一致；
 *   - parent 为 0 表示根级；非 0 时必须指向已存在的图层组；
 *   - clipping_source 非 0 时，该图层作为剪贴蒙版附着于指定图层。
 *
 * 所有写操作都在当前事务（comp_txn）内生效，否则自动开启隐式事务。
 */
#ifndef COMPOSITOR_COMP_LAYER_H
#define COMPOSITOR_COMP_LAYER_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comp_layer_desc {
  const char*     name;        /* 可选，UTF-8；NULL 表示自动命名 */
  comp_blend_mode blend;
  float           opacity;     /* 0..1 */
  comp_rect       transform;   /* 文档像素空间：原点 + 尺寸；w/h<=0 表示按像素尺寸 */
  float           rotation;    /* 度，顺时针 */
  uint8_t         flip_x;
  uint8_t         flip_y;
  uint8_t         is_group;    /* 1 = 图层组 */
  uint8_t         _reserved;   /* 对齐占位，必须为 0 */
  comp_layer_id   parent;      /* 0 = 根 */
  comp_sampling   sampling;    /* 采样模式；对齐上游 LayerTransform.sampling */
} comp_layer_desc;

/* 创建图层/图层组，返回新 id。 */
COMP_API comp_result comp_layer_add(comp_document document,
                                    const comp_layer_desc* desc,
                                    comp_layer_id* out_id);

/* 删除图层；若为图层组，其子树一并删除。 */
COMP_API comp_result comp_layer_remove(comp_document document, comp_layer_id id);

/* 把 id 移动到 before 之前（before 为 0 表示移到同级末尾）。 */
COMP_API comp_result comp_layer_reorder(comp_document document,
                                        comp_layer_id id,
                                        comp_layer_id before);

/*
 * 替换图层像素内容。pixels 可为 NULL 表示清空为全透明。
 *
 * 格式契约（写进 ABI 是因为宿主必须知道传什么、能读回什么）：
 *   - `COMP_PIXEL_RGBA8`：正常合成路径。
 *   - `COMP_PIXEL_RGBA16`：16 位图层。引擎保留 16 位资产，并为它维护一份
 *     8 位预乘副本（加约一阶噪声后取整，可复现），合成、缩略图与 8 位像素工具
 *     一律用那份副本——所以「传 16 位」与「传它的 8 位副本」合成结果一致。
 *     8 位工具作用于 16 位图层后，该图层就是 8 位（对齐上游 DeepLayers.swift:5）。
 *   - `COMP_PIXEL_RGBA16F`：HDR 图层（C 方案 C1）。同样是 8 字节/像素、预乘、
 *     native-endian，但每个通道是 IEEE 754 half float 而非 0..65535 整数，
 *     色彩空间为 extended sRGB（白 = 1.0，高光可 > 1.0）。引擎同样为它维护一份
 *     8 位工作副本：先把 half 的**值**按 0..65535 取整（white 以上的光在此截断，
 *     对齐上游 `DeepLayer.image(_:)`），再加约一阶噪声取整成 8 位。
 *     注意不要按 16 位整数的位模式直接读——那是 HDR 通路最典型的画面错误。
 *   - 未知格式 -> `COMP_ERR_UNSUPPORTED`，且不改动既有像素。
 *
 * `.comp` 存盘：RGBA8 图层写成 `.png`；RGBA16 与 RGBA16F 图层写成 `<uuid>.exr`
 * 资产（C3b，FLOAT / HALF 通道，逐位或保尾数），读盘按 `enginePixelFormat`
 * 声明校验。若宿主只接受 8 位资产，先跑 `COMP_TOOL_DEEP` 的 quantize，
 * 见 docs/tool-ops.md。
 */
COMP_API comp_result comp_layer_set_pixels(comp_document document,
                                           comp_layer_id id,
                                           const comp_image* image);

/* 设置/清除图层蒙版；mask 为 NULL 表示移除蒙版。 */
COMP_API comp_result comp_layer_set_mask(comp_document document,
                                         comp_layer_id id,
                                         const comp_mask* mask);

/*
 * 读取图层蒙版的存在与尺寸（与 set_mask 成对，供外壳面板回读与 .comp 往返）。
 *
 * 契约：
 *   - out_has_mask 非 0 = 该图层当前带蒙版；为 0 表示「没有蒙版」（此时
 *     out_width / out_height 均为 0，与域层空 MaskBuffer 同口径）；
 *   - 蒙版尺寸写 out_width / out_height（文档像素）。**像素不回传**——蒙版只是
 *     一张 8 位灰度覆盖图，要让宿主把它变成可见选区应走
 *     comp_selection_load_layer(COMP_SELECTION_LOAD_MASK)，那才是 source of truth
 *     的读出口；本入口只回答「有没有、多大」，因为外壳要的只是面板状态。
 *   - 只读：不开事务、不进撤销栈、不改动任何像素。
 *
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_get_mask(comp_document document,
                                         comp_layer_id id,
                                         int32_t* out_has_mask,
                                         int32_t* out_width,
                                         int32_t* out_height);

/*
 * 读取图层在树中的位置关系（is_group / parent / clipping_source）。
 *
 * 为什么必须补这个回读口：三者都是**已参与像素求值**的结构属性（组的隔离开关
 * 决定子树怎么合成、剪贴来源决定 coverage 从哪来），`comp_layer_desc` 只在
 * **创建时**给出它们，此后没有 setter 之外的回读路径。外壳要画「这一行是不是组」
 * 「这一层是否附着在下一层上（剪贴）」就只能靠猜——`.comp` 读回带组/带剪贴的文档
 * 后面板会静默失真。这与 S21 的 get_transform 是同一类缺口，一并补掉。
 *
 * 契约：
 *   - out_is_group 非 0 = 图层组；out_parent = 0 表示根级；
 *   - out_clipping_source = 0 表示不剪贴，否则是被附着的图层 id；
 *   - 三个出参都必填（只回一部分等于让壳在信息不全时做决定）；
 *   - **只读**：不开事务、不进撤销栈、不改动任何状态。
 *
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_get_group_info(comp_document document,
                                               comp_layer_id id,
                                               int32_t* out_is_group,
                                               comp_layer_id* out_parent,
                                               comp_layer_id* out_clipping_source);

/* 设置剪贴蒙版来源；source 为 0 表示取消。 */
COMP_API comp_result comp_layer_set_clipping_source(comp_document document,
                                                    comp_layer_id id,
                                                    comp_layer_id source);

/* ------------------------------------------------------ 图层属性（幂等） */

COMP_API comp_result comp_layer_set_blend(comp_document document,
                                          comp_layer_id id,
                                          comp_blend_mode blend);

COMP_API comp_result comp_layer_set_opacity(comp_document document,
                                            comp_layer_id id,
                                            float opacity);

COMP_API comp_result comp_layer_set_transform(comp_document document,
                                              comp_layer_id id,
                                              comp_rect transform,
                                              float rotation);

COMP_API comp_result comp_layer_set_visible(comp_document document,
                                            comp_layer_id id,
                                            int32_t visible);

/* 读取图层可见性（1 = 可见，0 = 隐藏）；与 set_visible 成对，供外壳面板回读。 */
COMP_API comp_result comp_layer_get_visible(comp_document document,
                                            comp_layer_id id,
                                            int32_t* out_visible);

/*
 * 读取图层变换（原点 + 尺寸 + 旋转角）；与 set_transform 成对，供外壳做相对步进
 * （缩放须以当前中心为锚、旋转须在既有角度上累加，两端都必须知道现值）。
 *
 * 契约：回读值可直接再次传入 comp_layer_set_transform（往返保真，见
 * docs/tool-ops.md 的往返口径）；filp_x / flip_y 不属于本入口——它们只在
 * comp_layer_desc 里给出，且 set_transform 不会覆盖（写入变换不改变既有翻转）。
 * 图层不存在 -> COMP_ERR_NOT_FOUND；出参为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_get_transform(comp_document document,
                                              comp_layer_id id,
                                              comp_rect* out_transform,
                                              float* out_rotation);

/* 设置/读取采样模式（对齐上游 LayerTransform.sampling）。 */
COMP_API comp_result comp_layer_set_sampling(comp_document document,
                                             comp_layer_id id,
                                             comp_sampling sampling);

COMP_API comp_result comp_layer_get_sampling(comp_document document,
                                             comp_layer_id id,
                                             comp_sampling* out_sampling);

/* ------------------------------------------------------ 调整图层与效果 */

/*
 * 调整图层：kind 见 .comp 规范（与 LayerAdjustment 枚举一致），
 * settings 为 UTF-8 JSON 串，键名沿用既有参数模型，向后兼容扩展。
 * 例如："{\"exposure\":0.35,\"contrast\":12.0}"。
 */
COMP_API comp_result comp_layer_add_adjustment(comp_document document,
                                               comp_layer_id target,
                                               uint32_t kind,
                                               const char* settings_json_utf8);

/* 只替换首个调整的 settings，kind 沿用既有值（与 :221 的 update_adjustment_at 相对）。 */
COMP_API comp_result comp_layer_update_adjustment(comp_document document,
                                                  comp_layer_id id,
                                                  const char* settings_json_utf8);

/*
 * 图层效果：effect 为效果枚举（描边/投影/内阴影/外发光/内发光 等），
 * settings 为 UTF-8 JSON 串。
 *
 * 契约说明（与 comp_layer_add_adjustment 的差别，勿混用）：
 *   - effect 是 .comp 格式的引擎扩展键（engineEffects[].effect），内核目前
 *     不参与效果像素求值，因此**刻意不做枚举白名单校验**，以免与 .comp 互通
 *     冲突——上游新增效果类型时旧内核仍应能原样保存与回传。
 *   - settings_json_utf8 同理不校验结构，按不透明字符串原样落盘。
 *   调用方应自行保证 effect 取值来自当前格式规范；非法值不会被拒绝，
 *   但也不会参与渲染。
 */
COMP_API comp_result comp_layer_set_effect(comp_document document,
                                           comp_layer_id id,
                                           uint32_t effect,
                                           const char* settings_json_utf8);

/* 移除指定效果。 */
COMP_API comp_result comp_layer_remove_effect(comp_document document,
                                              comp_layer_id id,
                                              uint32_t effect);

/* ------------------------------------------------------------ 只读查询 */

/* 图层数量（含组与子树）。 */
COMP_API comp_result comp_layer_count(comp_document document, size_t* out_count);

/* 按堆叠序返回图层 id 列表。ids 为空时仅返回数量（用于两段式查询）。 */
COMP_API comp_result comp_layer_list(comp_document document,
                                     comp_layer_id* ids,
                                     size_t capacity,
                                     size_t* out_count);

COMP_API comp_result comp_layer_get_opacity(comp_document document,
                                            comp_layer_id id,
                                            float* out_opacity);

COMP_API comp_result comp_layer_get_blend(comp_document document,
                                          comp_layer_id id,
                                          comp_blend_mode* out_blend);

/* ------------------------------------------------ 调整参数元数据（只读，供面板生成控件） */

/* 调整类型编号：与 .comp 的 engineAdjustments[].kind 及上游 `AdjustmentKind` 声明顺序一致。 */
typedef enum comp_adjustment_kind {
  COMP_ADJUSTMENT_HUE_SATURATION = 0,
  COMP_ADJUSTMENT_LEVELS = 1,
  COMP_ADJUSTMENT_CURVES = 2,
  COMP_ADJUSTMENT_EXPOSURE = 3,
  COMP_ADJUSTMENT_GRADIENT_MAP = 4,
  COMP_ADJUSTMENT_GRAIN = 5,
  COMP_ADJUSTMENT_ADD_NOISE = 6,
  COMP_ADJUSTMENT_GAUSSIAN_BLUR = 7,
  COMP_ADJUSTMENT_MOTION_BLUR = 8,
  COMP_ADJUSTMENT_INVERT = 9,
  COMP_ADJUSTMENT_BLACK_WHITE = 10,
  COMP_ADJUSTMENT_COLOR_BALANCE = 11,
} comp_adjustment_kind;

#define COMP_ADJUSTMENT_KIND_COUNT 12

/* 参数类型。 */
typedef enum comp_param_type {
  COMP_PARAM_NUMBER = 0,
  COMP_PARAM_BOOL = 1,
} comp_param_type;

/*
 * 单个可编辑参数的元数据。`json_path` 是相对 settings 对象的点分键路径，
 * 逐字取自上游 `LayerAdjustment` 的字段名（如 "exposureSettings.gamma"）；
 * 外壳据此生成控件，并把值写回同一路径，保证 `.comp` 往返保真。
 */
typedef struct comp_adjustment_param {
  const char* json_path;
  const char* label;
  comp_param_type type;
  double min_value;
  double max_value;
  double default_value;
  uint32_t flags; /* 保留，恒为 0 */
} comp_adjustment_param;

/* kind 的规范名称（= 上游 rawValue）；kind 非法时返回 COMP_ERR_INVALID_ARGUMENT。 */
COMP_API comp_result comp_layer_adjustment_kind_name(uint32_t kind, const char** out_name);

/* 该 kind 可编辑参数数量（0 表示无逐参数面板，如 Invert / Levels / Curves）。 */
COMP_API comp_result comp_layer_adjustment_param_count(uint32_t kind, size_t* out_count);

/* 取第 index 个参数的元数据（index 越界返回 COMP_ERR_INVALID_ARGUMENT）。 */
COMP_API comp_result comp_layer_adjustment_param_at(uint32_t kind,
                                                    size_t index,
                                                    comp_adjustment_param* out_param);

/* 只校验不写入：kind 合法且 settings 通过键名白名单与取值范围校验时返回 COMP_OK。 */
COMP_API comp_result comp_layer_adjustment_validate(uint32_t kind,
                                                    const char* settings_json_utf8);

/* 图层上的调整数量（供面板列出既有调整；只读，不开事务）。 */
COMP_API comp_result comp_layer_adjustment_count(comp_document document,
                                                 comp_layer_id id,
                                                 size_t* out_count);

/*
 * 按序读取第 index 个调整：写回 kind 与 settings JSON（UTF-8，不带 NUL 的长度写 out_size）。
 * 两段式：settings_json_utf8 为 nullptr 或 capacity 为 0 时只回报长度。
 */
COMP_API comp_result comp_layer_adjustment_at(comp_document document,
                                              comp_layer_id id,
                                              size_t index,
                                              uint32_t* out_kind,
                                              char* settings_json_utf8,
                                              size_t capacity,
                                              size_t* out_size);

/*
 * 按索引替换第 index 个调整（kind 与 settings 一并替换，校验同 add）。
 * 供面板编辑任意一条既有调整；comp_layer_update_adjustment 只作用于首个，两者并存。
 */
COMP_API comp_result comp_layer_update_adjustment_at(comp_document document,
                                                     comp_layer_id id,
                                                     size_t index,
                                                     uint32_t kind,
                                                     const char* settings_json_utf8);

/*
 * 从 settings JSON 按 json_path（= 键名表里的点分路径）读标量叶子，供面板回填既有值：
 * 命中数值写 out_number、命中布尔写 out_boolean（0/1）；路径缺失或非标量返回 COMP_ERR_NOT_FOUND。
 */
/*
 * 按索引删除第 index 个调整；越界返回 COMP_ERR_INVALID_ARGUMENT。
 * 与 add / update_at 对称，供面板删除既有调整。
 * ABI 尾部追加，冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_remove_adjustment(comp_document document,
                                                  comp_layer_id id,
                                                  size_t index);

COMP_API comp_result comp_layer_adjustment_read_value(const char* settings_json_utf8,
                                                      const char* json_path,
                                                      double* out_number,
                                                      int32_t* out_boolean);

/*
 * 由面板扁平值构造 settings JSON（引擎侧组装，外壳不写 JSON）：
 * values[i] 对应 param_at(kind, i)，ASCII 文本（布尔 "0"/"1"），空串 = 沿用缺省。
 * values 可少于 param_count（缺的按空串处理）。两段式：capacity 为 0 时只回报长度。
 */
COMP_API comp_result comp_layer_adjustment_build_settings(uint32_t kind,
                                                          const char* const* values,
                                                          size_t value_count,
                                                          char* settings_json_utf8,
                                                          size_t capacity,
                                                          size_t* out_size);

/*
 * 组隔离开关（仅图层组有意义；非组图层忽略，见 domain::Layer::isolated）：
 *   isolated = 0（默认）穿透：组内子层直接叠加到组外背景，混合模式与不透明度
 *                        穿透组边界（对齐上游 LayerGroups.swift）；
 *   isolated = 1 隔离：子树先自合成到一张透明缓冲，再作为一个整体参与混合。
 * 写入非组图层不报错但属性不参与合成（与 domain 层语义一致）。
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_set_isolated(comp_document document,
                                             comp_layer_id id,
                                             int32_t isolated);

/* 读取组隔离开关（1 = 隔离，0 = 穿透）；与 set_isolated 成对，供外壳面板回读与落盘往返。 */
COMP_API comp_result comp_layer_get_isolated(comp_document document,
                                             comp_layer_id id,
                                             int32_t* out_isolated);

/* ------------------------------------------------------- 合并 / 拼合 */

/*
 * 向下合并 / 组合并（对齐上游 ⌘E 家族）：把 id 指向的图层按「画布呈现」合成
 * 一个像素层——混合模式、不透明度、蒙版、剪贴蒙版、变换全部烘入，结果裁剪到
 * 内容并落在锚点槽位；id 为组时合并其全部后代且组本身消失（Merge Group）。
 * 整次合并记为历史里的一步撤销（加入调用方已开启的显式事务，或自成一个隐式步）。
 *
 * 精确性契约（渲染不变）：合并前后整篇合成逐字节一致。成立条件——被合并图层
 * 上方所有穿透组不透明度为 100%（穿透组按子层逐个缩放，两层并一层后无等价单层
 * 表示；隔离组不受限）。
 *
 * 错误码：id 不存在 -> COMP_ERR_NOT_FOUND；无事可合并（孤立图层、下方是组、
 * 组内无像素层、上方穿透组不透明度非 100%）-> COMP_ERR_STATE；被合并图层携带
 * 调整/效果 -> COMP_ERR_UNSUPPORTED（像素求值属滤镜图里程碑，见 compositor.h
 * 声明，在求值落地前明确拒绝，不悄悄丢参数也不假装烘入）。
 */
COMP_API comp_result comp_layer_merge_down(comp_document document, comp_layer_id id);

/* 合并计划查询：1 = 可合并（供外壳置灰菜单；纯判定，不合成、不改状态）。 */
COMP_API comp_result comp_layer_can_merge_down(comp_document document,
                                               comp_layer_id id,
                                               int32_t* out_can);

/*
 * 设置活动图层（外壳面板选中即写回；id 不存在返回 COMP_ERR_NOT_FOUND）。
 * 选择变更不产生撤销步（与上游一致：选中态不入历史），但随 .comp 的
 * activeLayerID 往返持久化。
 */
COMP_API comp_result comp_layer_set_active(comp_document document, comp_layer_id id);

/*
 * 拼合图像（对齐上游 Flatten Image）：整篇文档合成为铺满画布的单一像素层，
 * 一步撤销。错误码：空文档 -> COMP_ERR_STATE；携带调整/效果 ->
 * COMP_ERR_UNSUPPORTED（同 comp_layer_merge_down 的边界）。
 */
COMP_API comp_result comp_document_flatten(comp_document document);

/* --------------------------------------------------------- 工具操作 */

/*
 * 工具操作：把 engine/imaging 下 10 组上游像素内核接到宿主可调用的通路上。
 *
 * 背景（为什么有这组）：上游 10 组像素内核（魔棒/修补/抖动/透镜/画笔/16 位深度等）
 * 在本仓已实现且有测试覆盖，但历史上**没有任何 comp_* 入口能触发它们**——有实现、
 * 无功能。缺的不是把函数调用起来，而是参数契约：笔压、选区、容差、种子、蒙版来源
 * 在上游是工具层的交互概念。本组接口把这些概念收敛成稳定结构，由引擎装配成
 * 内核签名要的形态（详见 docs/tool-ops.md）。
 *
 * 通用契约：
 *   - 一步撤销（加入调用方已开启的显式事务，或自成一个隐式步）；
 *   - 参数闸在开事务**之前**：非法参数不留半成品、不进撤销栈；
 *   - 写入类算子不改 transform / blend / opacity，故合并、.comp 往返的
 *     既有不变性不受影响；
 *   - `note` 是非致命说明（UTF-8，如「GLYPHS 缺字形图集已回落菱形网点」）；
 *     致命原因写 comp_engine_last_error，此时返回非 0。
 */
typedef enum comp_tool_op {
  COMP_TOOL_NONE  = 0,
  COMP_TOOL_SELECT = 1, /* 魔棒 / 色彩范围 -> 累积写入图层蒙版 */
  COMP_TOOL_STROKE = 2, /* 笔刷 -> 落笔到图层像素 */
  COMP_TOOL_HEAL   = 3, /* 修补 -> 按 coverage 替换图层像素 */
  COMP_TOOL_DITHER = 4, /* 抖动 / 半调 -> 图层像素 */
  COMP_TOOL_LENS   = 5, /* 透镜畸变 -> 图层像素 */
  COMP_TOOL_DEEP   = 6, /* 深度保持 -> 16 位资产降位 */
  /*
   * F3 修饰类工具。数值从 9 起：7/8 已被域层的 scanlines/chunky 占用但未进
   * ABI，复用会造成「同一数值在两层指不同算子」。
   */
  COMP_TOOL_CLONE    = 9,  /* 仿制图章 -> 从快照（当前层或整篇合成）拷贝像素 */
  COMP_TOOL_SMUDGE   = 10, /* 涂抹 -> carried 缓冲拖色 */
  COMP_TOOL_LIQUIFY  = 11, /* 液化 Push -> 前向位移重采样 */
  COMP_TOOL_BLUR     = 12, /* 模糊 -> 快照模糊后按笔迹盖回 */
  /*
   * S9 尾部追加：渐变填充（上游 GradientTool）。
   * 与前 12 个算子不同的地方：它**不吃笔迹折线**，而是由两个端点定义的
   * 方向/半径 + 两个色标在图层像素上铺一片渐变；选区活跃时按选区覆盖率
   * 混合（与像素滤镜同口径）。见 `comp_tool_gradient`。
   */
  COMP_TOOL_GRADIENT = 13,
  /*
   * S10 尾部追加：形状工具（上游 ShapeTool 的矩形/椭圆/直线 + 通用贝塞尔路径）。
   * 与前 13 个算子不同的地方：它不吃笔迹折线，几何由 `comp_tool_shape` 直接给出
   * （对角点 / 两端点 / 路径段），填充与描边是同一份内核产出的两张覆盖率。
   * 见 `comp_tool_shape`。
   */
  COMP_TOOL_SHAPE = 14,
  /*
   * S11 尾部追加：文字工具（上游 TypeTool 的 ASCII 子集）。
   * 与前 14 个算子不同的地方：它不吃笔迹折线也不吃几何，文本 + 字号/行距/字距
   * 直接排版成覆盖率；字体是**引擎自带的 5×7 位图 ASCII 字体**，UTF-8/CJK
   * 明确不支持（参数闸拒绝，不静默画方框）。见 `comp_tool_type`。
   */
  COMP_TOOL_TYPE = 15
} comp_tool_op;

/* 选区参数（魔棒 / 色彩范围）。取值语义逐字对齐 WandPixels.h。 */
typedef struct comp_tool_select {
  int32_t tolerance;   /* 逐通道距离阈值 0..255（alpha 也参与比较） */
  int32_t radius;      /* 种子邻域半边长：参考色 = 该 (2r+1)^2 方窗的平均色 */
  int32_t contiguous;  /* 1 = 从种子 4-连通泛洪（种子不匹配则选中 0）；0 = 全图同色 */
  int32_t invert;      /* 1 = 选中取反 */
  int32_t use_color_range; /* 1 = 走 Color Range（按 include/exclude 列表）而非魔棒 */
  int32_t fuzziness;   /* Color Range 的逐通道容差 0..255 */
  int32_t seed_x;      /* 种子，文档像素坐标；必须落在目标图层内 */
  int32_t seed_y;
  /* include / exclude 为直通 sRGB 三元组（各 3*count 字节），各至多 8 色。 */
  const uint8_t* include;
  int32_t         include_count;
  const uint8_t* exclude;
  int32_t         exclude_count;
} comp_tool_select;

/* 笔刷参数。pressure 与 flow 相乘作为单次落笔的覆盖率上限（上游画笔口径）。 */
typedef struct comp_tool_brush {
  uint8_t color[3];  /* 直通 sRGB 笔画色 */
  float   diameter;  /* 笔迹直径（图层像素），>0 */
  float   pressure;  /* 笔压 0..1 */
  float   hardness;  /* 0..1，1 = 全实，0 = 从笔芯就羽化 */
  float   flow;      /* 0..1，覆盖率上限（防止高笔压涂死细节） */
  uint32_t seed;     /* 固定以保证同参数同结果 */
  /*
   * 颗粒（NoisePixels.h `noise_add_at`）。`grain_amount` 是 Photoshop 百分比
   * 口径（均匀噪声 ±amount% 半程，高斯标准差为其 2/3），0 = 不加颗粒。
   *
   * 颗粒叠在**落笔之后、整图层范围**，不是只在笔画上。噪声按文档坐标定，
   * 因此图层平移后同一笔仍得到同样的颗粒（否则颗粒会跟着图层原点跑）。
   * `grain_gaussian` 非 0 = 高斯；`grain_monochromatic` 非 0 = 三通道同值。
   */
  float   grain_amount;        /* 0..100，0 = 不加 */
  int32_t grain_gaussian;
  int32_t grain_monochromatic;
} comp_tool_brush;

/* 修补参数（Spot Heal / Content-Aware Fill）。 */
typedef struct comp_tool_heal {
  float   opacity; /* 0..1，结果按 coverage x opacity 替换 */
  int32_t mode;    /* 0 = Content-Aware（PatchMatch）；1 = Create Texture；2 = Proximity Match */
  uint32_t seed;   /* 固定以保证同参数同结果 */
} comp_tool_heal;

/* 抖动 / 半调参数（DitherParams 的引擎侧子集；字形图集属宿主资源，不由 ABI 灌入）。 */
typedef struct comp_tool_dither {
  int32_t style;   /* 0..9 = Atkinson/Floyd-Steinberg/Bayer2/4/8/Dots/Lines/Diamonds/Patterns/Glyphs */
  int32_t levels;  /* 2..16 阶（内核自身上限 16；2 = 纯 1-bit） */
  float   diffusion; /* 0..1，误差扩散强度 */
  float   density;   /* -1..1，加深（负）/减淡（正） */
  float   contrast;  /* -1..1 */
  int32_t cell;      /* 半调单元边长（像素）2..512 */
  float   angle;     /* 半调网屏角度（弧度） */
  int32_t light_on_dark;   /* 半调在暗色上标亮调而非在亮色上标暗调 */
  int32_t original_colors; /* 1 = 保留图像原色而非 dark/light 二色 */
  uint8_t dark[3];
  uint8_t light[3];
} comp_tool_dither;

/* 透镜畸变参数。k > 0 拉直桶形（边角裁切），k < 0 拉直枕形，k = 0 恒等。 */
typedef struct comp_tool_lens { double k; } comp_tool_lens;

/*
 * 仿制图章参数（字义见 domain/tool_ops.h 的 CloneParams）。
 *
 * `source_x/source_y` 是文档像素坐标的吸管落点。一次笔画的首笔偏移 =
 * `round(source - first_point)`（x、y 各自取整）；`aligned != 0 && has_offset != 0`
 * 时改用宿主带回的 `offset_x/offset_y`，于是同一会话的后续笔画保持首笔偏移。
 * 源快照在笔画开始时取好：`sample_all_layers` 取整篇合成，否则取目标图层。
 *
 * 笔刷直径/硬度/pressure/flow 复用 `brush`；`opacity` 再乘进 coverage 峰值。
 * 源像素越界时跳过（不会把图层擦出洞）。零初始化 = opacity 0 = 不动。
 */
typedef struct comp_tool_clone {
  double  source_x;
  double  source_y;
  int32_t has_source;      /* 1 = source_x/y 有效（否则必须已有 carried offset） */
  double  offset_x;        /* aligned 时沿用的「源 - 首笔落点」偏移（文档像素） */
  double  offset_y;
  int32_t has_offset;      /* 1 = offset_x/y 有效 */
  int32_t aligned;         /* 1 = 同一会话保持首笔偏移；0 = 每笔按 source 现算 */
  int32_t sample_all_layers; /* 1 = 整篇合成取样；0 = 只取目标图层 */
  float   opacity;         /* 0..1，笔迹总强度 */
} comp_tool_clone;

/*
 * Smudge / Liquify（Push）参数。直径与硬度取 `brush.diameter` / `brush.hardness`
 * （上游夹到 0.98），`strength` 是推动/拖色的强度（上游 settings.opacity）。
 * 零初始化 = strength 0 = 逐字节不动。
 */
typedef struct comp_tool_warp { float strength; } comp_tool_warp;

/*
 * 模糊工具参数（上游 BlurTool.swift `blurSample(for:)` 的 blurRadius）。
 *
 * 模糊样本在**一笔开始时**取快照：先把整层像素模糊成副本，再沿折线按笔刷
 * coverage 把副本盖回当前像素。同一处反复涂抹会继续变软（每笔都以当笔开始
 * 时的像素为准），与上游一致。
 *
 * `radius` 0.5..50 是高斯半径（图层像素）；实际 sigma 再夹到图层较长边的
 * 一半（上游 `sidePixels / 2`）。几何复用 `stroke_points` 与
 * `brush.diameter/hardness/pressure/flow`；零半径经参数闸拒绝。
 */
typedef struct comp_tool_blur { float radius; } comp_tool_blur;

/*
 * 渐变参数（上游 GradientTool 的「线性 / 径向 + 两个色标」子集）。
 *
 * 端点用**文档像素坐标**：`(start_x, start_y)` 是第一色标所在处，
 * `(end_x, end_y)` 是第二色标（线性）或半径终点（径向）。图层像素按
 * `effective_bounds()` 的原点换算到文档坐标后求 t（与 stroke/clone 同一套
 * 平移口径；缩放/旋转图层不做逆变换，与其余工具算子一致）。
 *
 * 色标是**直通 sRGB + alpha**（各 4 字节，非预乘），铺到图层时按 source-over
 * 预乘混合；`opacity` 再乘进覆盖率。选区活跃时覆盖率 = 选区有效 coverage，
 * 因此「先套选区再拉渐变」只改选区内像素。
 *
 * 端点重合（线性方向长度为 0 / 径向半径 0）被参数闸拒绝：那不是一个可定义的
 * 渐变，静默画成纯色会让宿主以为方向生效了。
 */
typedef struct comp_tool_gradient {
  int32_t type;        /* 0 = 线性，1 = 径向 */
  double  start_x;     /* 第一色标，文档像素坐标 */
  double  start_y;
  double  end_x;       /* 第二色标（线性）/ 半径终点（径向） */
  double  end_y;
  uint8_t color0[4];   /* 直通 sRGB + alpha（0..255） */
  uint8_t color1[4];
  float   opacity;     /* 0..1，整片渐变的覆盖率上限 */
} comp_tool_gradient;

/* ============================================================== 形状（S10） */

/* 形状种类（数值与域层 `ShapeKind`、形状内核 `ShapeRasterKind` 三方一致）。 */
typedef enum comp_shape_kind {
  COMP_SHAPE_RECT = 0,
  COMP_SHAPE_ELLIPSE = 1,
  COMP_SHAPE_LINE = 2,
  COMP_SHAPE_PATH = 3
} comp_shape_kind;

/*
 * 三次贝塞尔路径段（文档像素）。`new_subpath` 非 0 表示从 `start` 起一条新子路径，
 * 否则接着上一条子路径的终点画。坐标是 float（与 `comp_selection_cubic` 同口径），
 * 引擎侧转 double 后做 de Casteljau 细分，0.1 像素弦高容差。
 */
typedef struct comp_shape_cubic {
  float   start[2];
  float   control1[2];
  float   control2[2];
  float   end[2];
  int32_t new_subpath;
} comp_shape_cubic;

/*
 * 形状样式（语义见 domain/tool_ops.h 的 `ShapeStyle`）。
 *
 * 颜色是**直通 sRGB + alpha（非预乘）**，铺到图层时按 source-over 预乘混合；
 * `opacity` 再乘进逐像素覆盖率；`antialias` 非 0 = 抗锯齿（4 行超采样 +
 * 水平解析覆盖率），0 = 像素中心一票（硬边）。
 *
 * `stroke_width` 是文档像素的**全宽**（胶囊描边半径 = 半宽），圆头端点/圆角连接。
 * 矩形/椭圆描边的是轮廓本身；直线只有描边。
 */
typedef struct comp_shape_style {
  int32_t fill_enabled;   /* 非 0 = 填充 */
  int32_t stroke_enabled; /* 非 0 = 描边 */
  uint8_t fill_color[4];  /* 直通 sRGB + alpha（0..255） */
  uint8_t stroke_color[4];
  double  stroke_width;   /* 文档像素全宽；只在 stroke_enabled 非 0 时校验（> 0） */
  float   opacity;        /* 0..1 */
  int32_t antialias;      /* 非 0 = 抗锯齿 */
} comp_shape_style;

/*
 * 形状参数（上游 ShapeTool.swift 的语义；三处与本仓的显式差异见
 * domain/tool_ops.h 的 `ShapeParams` 注释：画进当前图层而不是新建图层、
 * 填充与描边各自独立、多出通用贝塞尔路径）。
 *
 * 几何：
 *   - COMP_SHAPE_RECT / COMP_SHAPE_ELLIPSE 用对角点 (x0,y0)-(x1,y1)，顺序任意；
 *     rect 的 `corner_radius` 夹到短边一半，<= 0 为直角；
 *   - COMP_SHAPE_LINE 的 (x0,y0)-(x1,y1) 是两端点，恒为描边：
 *     `style.fill_enabled` 必须为 0，`style.stroke_enabled` 必须非 0；
 *   - COMP_SHAPE_PATH 用 `segments`（1..65536 段），填行走非零缠绕（反向子路径成洞），
 *     `closed` 只影响描边是否回到起点；
 *   - 全部坐标是**文档像素**，引擎减掉图层 `effective_bounds()` 原点后落图层局部像素；
 *   - 形状与目标图层完全不相交时：成功、**无像素改动且不产生撤销步**，note 如实说明。
 *
 * 参数闸（COMP_ERR_INVALID_ARGUMENT，且不留半成品/不进撤销栈）：kind 越界、
 * 填充与描边全关、可见色全透明（alpha 0）、opacity 不在 0..1 或为 0、
 * 描边开启时 stroke_width 非正（纯填充形状不看该字段，0 合法）、
 * 坐标为 NaN/Inf 或绝对值 > 1e6、rect/ellipse 宽或高为 0、
 * line 两端点重合、path 段数为 0 或超限、path 坐标非法。
 *
 * 16 位图层：与其余工具算子同口径，经 8 位工作副本穿过（不静默降位整层，
 * 结果按 `merge_deep_paint` 合回）；RGBA16F 拒绝（COMP_ERR_STATE）。
 */
typedef struct comp_tool_shape {
  comp_shape_kind kind;
  double  x0;             /* rect/ellipse：对角点；line：起点 */
  double  y0;
  double  x1;             /* rect/ellipse：对角点；line：终点 */
  double  y1;
  double  corner_radius;  /* rect 专用，文档像素；夹到短边一半 */
  const comp_shape_cubic* segments; /* path 专用 */
  int32_t segment_count;
  int32_t closed;         /* path 描边是否闭合（填充总是隐式闭合） */
  comp_shape_style style;
} comp_tool_shape;

/* ============================================================== 文字（S11） */

/*
 * 文字样式（语义见 domain/tool_ops.h 的 `TypeStyle`）。
 *
 * `font_size` 是**单元高**（文档像素）：引擎的自带位图字体是 5 列 × 7 行，
 * 缩放 = font_size / 7。`line_height` 是行距倍数（>= 1，上限 8）；`letter_spacing`
 * 是附加字距（文档像素，可负，但「6×缩放 + 字距」必须 >= 0.5）。
 * `antialias` 非 0 = 逐像素精确面积覆盖率；0 = 像素中心一票（硬边）。
 */
typedef struct comp_type_style {
  double font_size;      /* 单元高，文档像素；6..2048 */
  double line_height;    /* 行距倍数；1..8（< 1 会让相邻两行字身重叠） */
  double letter_spacing; /* 附加字距，文档像素；|x| <= 4096 且步进为正 */
  int32_t antialias;     /* 非 0 = 抗锯齿（精确面积） */
} comp_type_style;

/*
 * 文字参数（上游 TypeTool.swift 的 ASCII 子集；差异见 domain/tool_ops.h 的
 * `TypeParams` 注释：画进当前图层而不是新建可再编辑的文字图层、自带位图字体、
 * 只做左对齐 + 字号/行距/字距）。
 *
 * 文本：
 *   - `text` 是 UTF-8 缓冲区，但**只接受 0x20..0x7E 与 '\n'**；其它字节（含
 *     UTF-8 多字节 = CJK、Tab 等控制字符）被参数闸以 COMP_ERR_INVALID_ARGUMENT
 *     拒绝。本仓零依赖、不链接字体引擎，没有 CJK 字形就不假装有；
 *   - `text_length` 是字节数（ASCII 下等于字符数），上限 65536；
 *   - `origin_x/origin_y` 是文本框**左上角**（文档像素，可带小数）。
 *
 * 排版：单字形步进 = 6×缩放 + letter_spacing；行步进 = font_size × line_height；
 * 文本框宽 = 最长行的步进宽度，高 = font_size + (行数-1)×行步进。全部坐标是
 * **文档像素**，引擎减掉图层 `effective_bounds()` 原点后落图层局部像素。
 *
 * 颜色是**直通 sRGB + alpha（非预乘）**，铺到图层时按 coverage/255 × color.a/255
 * × opacity 走预乘 source-over；选区活跃时再乘选区有效 coverage（覆盖率 0 的
 * 像素逐字节不动）。
 *
 * 参数闸（COMP_ERR_INVALID_ARGUMENT，且不留半成品/不进撤销栈）：文本为空、
 * 只含换行、含非可打印 ASCII、超过 65536 字符、字号/行距/字距越界或非有限、
 * 字距把步进压到 0.5 px 以下、原点 NaN/Inf 或绝对值 > 1e6、颜色 alpha 为 0、
 * opacity 不在 0..1 或为 0。
 *
 * 与图层完全不相交时：成功、**无像素改动且不产生撤销步**，note 如实说明。
 * 16 位图层经 8 位工作副本穿过；RGBA16F 拒绝（COMP_ERR_STATE）。
 */
typedef struct comp_tool_type {
  double origin_x;          /* 文本框左上角，文档像素 */
  double origin_y;
  const char* text;         /* 仅 0x20..0x7E 与 '\n'；UTF-8 多字节被拒绝 */
  int32_t text_length;      /* 字节数；1..65536 */
  comp_type_style style;
  uint8_t color[4];         /* 直通 sRGB + alpha（0..255） */
  float opacity;            /* 0..1，再乘进逐像素覆盖率 */
} comp_tool_type;

/*
 * 一次工具操作。零初始化即可调用（各子结构缺省即「什么都不改」）。
 * `stroke.points` 为文档像素坐标折线（x, y 成对，共 point_count*2 个 float）。
 * `heal.coverage` 为 8 位灰度蒙版（255 = 待修补），尺寸与图层像素一致，
 * 按 `heal.coverage_stride`（字节/行，至少等于图层宽）逐行解释。
 */
typedef struct comp_tool_op_desc {
  comp_tool_op     kind;
  comp_tool_select select;
  comp_tool_brush  brush;
  const float*     stroke_points;
  int32_t          stroke_point_count;
  comp_tool_heal   heal;
  const uint8_t*   heal_coverage;
  size_t           heal_coverage_stride;
  comp_tool_dither dither;
  comp_tool_lens   lens;
  /*
   * F3 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * clone 用 `stroke_points`（文档像素折线）与 `brush`；smudge/liquify 只需
   * `stroke_points`、`brush.diameter/hardness` 与 `warp.strength`。
   */
  comp_tool_clone  clone;
  comp_tool_warp   warp;
  /*
   * P1 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * 模糊工具用 `stroke_points`、`brush` 与 `blur.radius`。
   */
  comp_tool_blur   blur;
  /*
   * S5 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * 仅 COMP_TOOL_STROKE 读取：非 0 = 橡皮擦（destination-out，只减不增），
   * 0 = 画笔（source-over）。橡皮擦忽略 `brush.grain_*`（颗粒无像素可叠）。
   */
  int32_t          erase;
  /*
   * S9 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * 仅 COMP_TOOL_GRADIENT 读取：端点、两色标与不透明度，语义见 comp_tool_gradient。
   * 它不读 `stroke_points`（渐变由两个端点定义，不是折线）。
   */
  comp_tool_gradient gradient;
  /*
   * S10 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * 仅 COMP_TOOL_SHAPE 读取：几何（对角点 / 两端点 / 贝塞尔路径段）与
   * 填充/描边样式，语义见 comp_tool_shape。它同样不读 `stroke_points`。
   */
  comp_tool_shape  shape;
  /*
   * S11 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * 仅 COMP_TOOL_TYPE 读取：文本框原点、ASCII 文本、字号/行距/字距与颜色，
   * 语义见 comp_tool_type。它不读 `stroke_points`，也不读 `shape`。
   */
  comp_tool_type   type;
} comp_tool_op_desc;

/*
 * 执行一次工具操作。note 可为 NULL；非 NULL 时写入 UTF-8 非致命说明
 * （需要 '\0' 空间，两段式：note_capacity 为 0 时只回报所需长度，out_note_size
 * 写入含 '\0' 的长度）。
 *
 * 错误码（经本入口可达的只有下列四种）：
 *   id 不存在 -> COMP_ERR_NOT_FOUND；kind/参数非法 -> COMP_ERR_INVALID_ARGUMENT；
 *   组图层/无像素图层 -> COMP_ERR_STATE；内核内存不足 -> COMP_ERR_OUT_OF_MEMORY。
 *
 * 注意：deep 的 round trip 模式**刻意不经本 ABI 暴露**——`comp_tool_op_desc`
 * 不承载 `deep_mode`（它需要「一组 8 位操作结果集」，本结构体装不下），故经本
 * 入口只能做 16 位降位（quantize）。域层的 `ToolOpParams::deep_mode` 对 round trip
 * 有显式 `COMP_ERR_UNSUPPORTED` 拒绝，但该码在 ABI 边界不可达，宿主契约里不要写它。
 */
COMP_API comp_result comp_tool_apply(comp_document document,
                                     comp_layer_id target,
                                     const comp_tool_op_desc* desc,
                                     char* note,
                                     size_t note_capacity,
                                     size_t* out_note_size);

/* ============================================================ 只读查询 */

/*
 * 图层像素的 256 桶亮度直方图（上游 Levels 面板的直方图，`levels_histogram`）。
 *
 * `bins` 必须是 `double[256]`。`coverage` 可为 NULL（权重 1）；非 NULL 时按
 * `coverage_stride` 逐行取行，尺寸须不小于图层高、每行不小于图层宽。
 *
 * **只读**：不写像素、不写蒙版、不开事务、不进撤销栈。
 *
 * 关键口径：预乘像素**先反预乘再取亮度**，coverage 权重按 alpha 走——否则半透明
 * 区域的颜色被 alpha 压低，面板曲线系统性偏向暗部，与实际 levels 调整对不上。
 *
 * 错误码：`bins` 为 NULL -> COMP_ERR_INVALID_ARGUMENT；图层不存在 -> COMP_ERR_NOT_FOUND；
 * 图层无像素或是组 -> COMP_ERR_STATE；**16 位图层 -> COMP_ERR_UNSUPPORTED**（桶是 8 位的，
 * 先用 COMP_TOOL_DEEP 降位，本入口不偷偷降位）。
 */
COMP_API comp_result comp_layer_histogram(comp_document document,
                                          comp_layer_id target,
                                          const uint8_t* coverage,
                                          size_t coverage_stride,
                                          double* bins);

/*
 * 修补 coverage 的非零包围盒（`heal_coverage_bounds`），半开区间
 * `[bounds[0], bounds[2]) x [bounds[1], bounds[3])`，四个 `int32_t` 出参。
 * coverage 全 0 时四个值都是 0。**只读**，不开事务、不进撤销栈。
 *
 * 存在的意义：`spot_heal` 内部算的是同一份包围盒，暴露出来免得宿主自己再扫一遍
 * 然后与内核漂移。
 *
 * 错误码：出参为 NULL 或尺寸非正或 coverage 为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_tool_coverage_bounds(const uint8_t* coverage,
                                               size_t coverage_stride,
                                               int32_t width,
                                               int32_t height,
                                               int32_t bounds[4]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_LAYER_H */

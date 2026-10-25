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
 * docs/tool-ops.md 的往返口径）；flip_x / flip_y 不属于本入口——S24 起由
 * comp_layer_get_flip / set_flip 单独读写，set_transform 依旧不会覆盖既有翻转
 * （写入变换不改变翻转态，两者正交）。
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

/*
 * 设置/读取图层镜像（S24）：flip_x / flip_y 是对图层局部像素的显示态翻转，
 * 与 transform / rotation 正交（set_transform 不覆盖它们，翻转也不改 origin/size）。
 *
 * 为什么必须补这两个口：引擎的 resample 与图层求值一直在按 Layer::flip_x /
 * flip_y 翻转像素，`.comp` 的 transform.flipX / flipY 也一直往返，但 ABI 里只有
 * comp_layer_desc 能在**创建时**给出它们——外部既改不了也读不到，镜像成了「格式
 * 里有、界面没有」的半截能力；S21 的 get_transform 又刻意把翻转排除在外，于是
 * 外壳连「这一层现在是翻转态」都无从知道（面板画不出、菜单标不出）。
 *
 * 契约：
 *   - flip_x / flip_y 非 0 即翻转；写入时规范化为 0/1，回读只会是 0 或 1；
 *   - 两者同时为 1 = 水平 + 垂直（画面等价 180°，但 rotation 语义保持独立）；
 *   - 写入进撤销栈（与 set_visible / set_sampling 同一套隐式事务）；
 *   - 回读纯只读：不开事务、不进撤销栈、不改动任何状态；
 *   - 图层不存在 -> COMP_ERR_NOT_FOUND；出参为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 *
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_set_flip(comp_document document,
                                         comp_layer_id id,
                                         int32_t flip_x,
                                         int32_t flip_y);

COMP_API comp_result comp_layer_get_flip(comp_document document,
                                         comp_layer_id id,
                                         int32_t* out_flip_x,
                                         int32_t* out_flip_y);

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
 * 图层效果：effect 为效果枚举（描边/投影/内阴影/外发光/内发光/斜面和浮雕/
 * 颜色叠加/渐变叠加/图案叠加 等），settings 为 UTF-8 JSON 串。
 *
 * 契约说明（与 comp_layer_add_adjustment 的差别，勿混用）：
 *   - effect 是 .comp 格式的引擎扩展键（engineEffects[].effect），取值见
 *     compositing/effect_eval.h 的 kEffectStroke..kEffectPatternOverlay（0..9，与格式
 *     规范一一对应）。内核**不做枚举白名单校验**，以免与 .comp 互通冲突——上游
 *     新增效果类型时旧内核仍应能原样保存与回传；未知 kind 不参与像素求值。
 *   - settings_json_utf8 同理不校验结构，按不透明字符串原样落盘。
 *   - **效果参与像素求值**（S22 记录的「纯元数据」结论已在 S24 复核时推翻）：
 *     10 个 kind 由 compositing/effect_eval.cpp 的 apply_effects 就地求值（单趟），
 *     composite_ops.cpp 先用 effects_margin() 外扩包围盒再调用。回读见下方的
 *     comp_layer_effect_count / comp_layer_effect_at。
 */
COMP_API comp_result comp_layer_set_effect(comp_document document,
                                           comp_layer_id id,
                                           uint32_t effect,
                                           const char* settings_json_utf8);

/* 移除指定效果。 */
COMP_API comp_result comp_layer_remove_effect(comp_document document,
                                              comp_layer_id id,
                                              uint32_t effect);

/*
 * 效果回读（S24）：与 set_effect / remove_effect 成对。
 *
 * count + at 两段式，与 comp_layer_adjustment_count / comp_layer_adjustment_at
 * 同构：at 的 settings_json_utf8 为 nullptr 或 capacity 为 0 时只回报长度
 * （out_size），便于调用方先量后取；三个出参均可为 nullptr（全空 = 只探测该下标存在）。
 *
 * 为什么必须补：效果已经参与像素求值，但外壳只能写不能读——画不出「这层有几个
 * 效果」，菜单也无法标出已启用的项。set_effect 写入的设置串若没有回读口，外部
 * 只能自己记账，`.comp` 读回后记账即失真。
 *
 * 契约：
 *   - 顺序即 layer->effects 的写入序（与 .comp 的 engineEffects 数组同序）；
 *   - index 越界 -> COMP_ERR_INVALID_ARGUMENT；
 *   - 回读纯只读：不开事务、不进撤销栈；
 *   - 图层不存在 -> COMP_ERR_NOT_FOUND；out_count 为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 *
 * ABI 尾部追加：冻结面（前序函数与结构体布局）不变。
 */
COMP_API comp_result comp_layer_effect_count(comp_document document,
                                             comp_layer_id id,
                                             size_t* out_count);

COMP_API comp_result comp_layer_effect_at(comp_document document,
                                          comp_layer_id id,
                                          size_t index,
                                          uint32_t* out_effect,
                                          char* settings_json_utf8,
                                          size_t capacity,
                                          size_t* out_size);

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
  /*
   * 12…15 是本仓在 S27 追加的**破坏性**调整（Image > Adjustments 用），
   * 上游 `AdjustmentKind` 没有这四种，故不参与 .comp 的 kind 白名单互操作；
   * 作为调整图层使用时同样合法（它们的求值器只做逐像素映射）。
   */
  COMP_ADJUSTMENT_BRIGHTNESS_CONTRAST = 12,
  COMP_ADJUSTMENT_DESATURATE = 13,
  COMP_ADJUSTMENT_THRESHOLD = 14,
  COMP_ADJUSTMENT_POSTERIZE = 15,
  /*
   * 16…24 同样是本仓追加，补齐 Photoshop「Image > Adjustments」的其余条目，
   * 语义与 PS 同名调整一致（见 engine/imaging/AdjustPixels.h 的逐条说明）。
   */
  COMP_ADJUSTMENT_VIBRANCE = 16,
  COMP_ADJUSTMENT_SELECTIVE_COLOR = 17,
  COMP_ADJUSTMENT_PHOTO_FILTER = 18,
  COMP_ADJUSTMENT_CHANNEL_MIXER = 19,
  COMP_ADJUSTMENT_COLOR_LOOKUP = 20,
  COMP_ADJUSTMENT_SHADOWS_HIGHLIGHTS = 21,
  COMP_ADJUSTMENT_AUTO_TONE = 22,
  COMP_ADJUSTMENT_AUTO_CONTRAST = 23,
  COMP_ADJUSTMENT_AUTO_COLOR = 24,
} comp_adjustment_kind;

#define COMP_ADJUSTMENT_KIND_COUNT 25

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

/* 该 kind 可编辑参数数量（0 表示无逐参数面板，如 Invert / Curves）。 */
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
  COMP_TOOL_TYPE = 15,
  /*
   * P1 尾部追加：减淡 / 加深 / 海绵（上游工具箱的 Dodge / Burn / Sponge）。
   * 三者都吃 `stroke_points` 折线与 `brush` 几何（与模糊同款：coverage 决定
   * 强度），差别只在逐像素的色调运算；参数见 `comp_tool_tone`。
   * 语义：Dodge 提亮、Burn 压暗、Sponge 去色/加色；alpha 不变、全透明像素
   * 不动、选区外逐字节不动。
   */
  COMP_TOOL_DODGE = 16,
  COMP_TOOL_BURN = 17,
  COMP_TOOL_SPONGE = 18,
  /*
   * P1 尾部追加：历史记录画笔。从历史面板下标 `history.state` 指向的那一份
   * 历史状态里取样（同一 id 的图层），按 `stroke_points` 折线与 `brush` 几何
   * 刷回**当前**图层。语义见 `comp_tool_history_brush`。
   */
  COMP_TOOL_HISTORY_BRUSH = 19
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
 * 渐变种类（P1-7 尾部追加 2..4。0/1 的数值与像素语义不变）。
 */
typedef enum comp_gradient_type {
  COMP_GRADIENT_LINEAR    = 0, /* 线性：沿端点方向的投影比例 */
  COMP_GRADIENT_RADIAL    = 1, /* 径向：到起点的距离 / 端点距离 */
  COMP_GRADIENT_ANGLE     = 2, /* 角度（锥形）：绕起点的方位角 / 360° */
  COMP_GRADIENT_REFLECTED = 3, /* 对称：起点两侧镜像 |投影| */
  COMP_GRADIENT_DIAMOND   = 4  /* 菱形：|投影| + |横向| */
} comp_gradient_type;

/*
 * 多色标（P1-7 尾部追加）。`position` 是 0..1 的段位置，**必须严格升序**给出
 * （引擎拒绝乱序而不是替宿主排序：静默排序会把「填错了顺序」变成「画面上颜色
 * 不是我要的顺序」）；`midpoint` 是该色标到下一个色标之间的中点位置，取
 * (0,1) 开区间，0.5 = 线性；`color` 是直通 sRGB + alpha（非预乘）。
 *
 * 一次渐变最多 `COMP_TOOL_GRADIENT_MAX_STOPS` 个色标。
 */
#define COMP_TOOL_GRADIENT_MAX_STOPS 32

typedef struct comp_gradient_stop {
  double  position;   /* 0..1，严格升序 */
  double  midpoint;   /* (0,1)，0.5 = 线性 */
  uint8_t color[4];   /* 直通 sRGB + alpha（0..255） */
} comp_gradient_stop;

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
 *
 * P1-7：`type` 扩展为 0..4；多色标与抖动开关**不在本结构体里**，而是在
 * `comp_tool_op_desc` 尾部追加（`gradient_stops` / `gradient_stop_count` /
 * `gradient_dither`），这样 `color0/color1` 的偏移与旧宿主逐字节不变。
 * 多色标非空时完全替代 `color0/color1`。
 */
typedef struct comp_tool_gradient {
  int32_t type;        /* comp_gradient_type：0 线性 / 1 径向 / 2 角度 / 3 对称 / 4 菱形 */
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
 * 缩放 = font_size / 7。传入字体句柄（`type_font`）后 `font_size` 换成 **em 高**。
 * `line_height` 是行距倍数（>= 1，上限 8）；`letter_spacing`
 * 是附加字距（文档像素，可负，但字形步进必须 >= 0.5）。
 * `antialias` 非 0 = 抗锯齿；0 = 像素中心一票 / 灰度阈值 128（硬边）。
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
 * **一次性落墨 vs 可编辑图层**：本结构走 `comp_tool_apply`，把字形直接写进当前
 * 图层的像素，写完不可再编辑；要「文字对象 + 渲染缓存」并支持反复改文本/字号，
 * 用本头文件尾部的 `comp_layer_add_text` / `comp_layer_set_text`（P0-6 ①）。
 *
 * 文本：
 *   - 默认（`comp_tool_op_desc::type_font` 为 NULL）走内置 5×7 位图字体：
 *     `text` 是 UTF-8 缓冲区，但**只接受 0x20..0x7E 与 '\n'**；其它字节（含
 *     UTF-8 多字节 = CJK、Tab 等控制字符）被参数闸以 COMP_ERR_INVALID_ARGUMENT
 *     拒绝——没有 CJK 字形就不假装有；
 *   - 传入字体句柄后走字体轨道：`text` 按 UTF-8 解码（UTF-8 非法序列、Tab 等
 *     控制字符仍拒绝），CJK 可用；缺字只占步进、不画方框，note 回报缺字个数；
 *   - `text_length` 是字节数，上限 65536；
 *   - `origin_x/origin_y` 是文本框**左上角**（文档像素，可带小数）。
 *
 * 排版：默认轨道的单字形步进 = 6×缩放 + letter_spacing，`style.font_size` 是
 * 5×7 单元高；字体轨道的字形步进来自字体本身（`style.font_size` 是 **em 高**）。
 * 两条轨道的行步进都是 font_size × line_height；文本框宽 = 最长行的步进宽度。
 *
 * 颜色是**直通 sRGB + alpha（非预乘）**，铺到图层时按 coverage/255 × color.a/255
 * × opacity 走预乘 source-over；选区活跃时再乘选区有效 coverage（覆盖率 0 的
 * 像素逐字节不动）。
 *
 * 参数闸（COMP_ERR_INVALID_ARGUMENT，且不留半成品/不进撤销栈）：文本为空、
 * 只含换行、默认轨道含非可打印 ASCII（字体轨道：UTF-8 非法或含控制字符）、
 * 超过 65536 字符、字号/行距/字距越界或非有限、字距把步进压到 0.5 px 以下、
 * 原点 NaN/Inf 或绝对值 > 1e6、颜色 alpha 为 0、opacity 不在 0..1 或为 0。
 * 传入的字体句柄非 NULL 但通不过 magic 校验 -> COMP_ERR_STATE（不回落成「没传字体」；
 * 已释放的句柄不可再用，见 comp_font_release）。
 *
 * 与图层完全不相交时：成功、**无像素改动且不产生撤销步**，note 如实说明。
 * 16 位图层经 8 位工作副本穿过；RGBA16F 拒绝（COMP_ERR_STATE）。
 */
typedef struct comp_tool_type {
  double origin_x;          /* 文本框左上角，文档像素 */
  double origin_y;
  const char* text;         /* 默认轨道仅 0x20..0x7E 与 '\n'；字体轨道为 UTF-8 */
  int32_t text_length;      /* 字节数；1..65536 */
  comp_type_style style;
  uint8_t color[4];         /* 直通 sRGB + alpha（0..255） */
  float opacity;            /* 0..1，再乘进逐像素覆盖率 */
} comp_tool_type;

/* ============================================================== 色调（P1） */

/*
 * 减淡 / 加深 / 海绵参数（语义见 domain/tool_ops.h 的 `ToneParams`）。
 *
 * 三者共用 `stroke_points` 折线与 `brush` 的直径/硬度/pressure/flow（与模糊
 * 同一条笔迹通路），本结构只承载「哪一种色调运算、多强」：
 *
 * `mode` 的取值范围随算子不同：
 *   - COMP_TOOL_DODGE / COMP_TOOL_BURN：0 = 中间调（缺省，对齐上游工具栏）、
 *     1 = 阴影、2 = 高光。范围权重按亮度取 1-|2l-1| / (0.5-l)/0.5 / (l-0.5)/0.5，
 *     再过 smoothstep（线性边界会在中间调与高光交界留下可见台阶）。
 *   - COMP_TOOL_SPONGE：0 = 去色、1 = 加色。
 *
 * `strength` 0..1 是本笔总量（Dodge/Burn 是上游 Exposure，Sponge 是 Flow），
 * 再乘进 coverage 与选区有效覆盖率。参数闸拒绝 <= 0（恒等操作不记空撤销）；
 * alpha 保持不变，全透明像素（alpha 0）跳过。
 */
typedef struct comp_tool_tone {
  int32_t mode;
  float   strength;
} comp_tool_tone;

/*
 * 历史记录画笔参数（P1 尾部追加，COMP_TOOL_HISTORY_BRUSH）。
 *
 * `state` 是历史面板下标：0 = 文档最初状态，i = 第 i 步之后（与
 * `comp_document_history_step_count` / `comp_document_history_stats` 推出的
 * 面板编号同一套）。越界、或该状态里没有同 id 的
 * 图层（例如图层是后来新建的）→ 参数闸拒绝，不开事务、不改像素。
 *
 * 取样按 `effective_bounds` 原点平移（与渐变 / 仿制图章同一套口径，不认
 * 旋转）；历史图层若是 16 位 / HDR，按显示口径降到 8 位再取；落在历史图层
 * 像素之外视为透明。混合是预乘空间的线性插值：
 *   out = src * w + dst * (1 - w)，w = 笔迹 coverage × opacity × 选区覆盖率。
 *
 * `opacity` 0..1 为本笔总量，参数闸拒绝 <= 0（恒等操作不记空撤销）；逐字节
 * 比较后确实无改动（取样与当前像素相同）时同样按 noop 撤回事务。
 */
typedef struct comp_tool_history_brush {
  int32_t state;
  float   opacity;
} comp_tool_history_brush;

/*
 * 一次工具操作。零初始化即可调用（各子结构缺省即「什么都不改」）。
 * `stroke.points` 为文档像素坐标折线（x, y 成对，共 point_count*2 个 float）。
 * `heal.coverage` 为 8 位灰度蒙版（255 = 待修补），尺寸与图层像素一致，
 * 按 `heal.coverage_stride`（字节/行，至少等于图层宽）逐行解释；
 * `heal_coverage_rows` 为缓冲实际行数（0 = 旧宿主，按图层高信任）。
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
  /*
   * P1 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * COMP_TOOL_DODGE / _BURN / _SPONGE 读取：模式与强度，语义见 comp_tool_tone；
   * 笔迹折线与笔刷几何仍走 `stroke_points` 与 `brush`。
   */
  comp_tool_tone   tone;
  /*
   * P1 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   * COMP_TOOL_HISTORY_BRUSH 读取：取样状态下标与不透明度，语义见
   * comp_tool_history_brush；笔迹折线与笔刷几何仍走 `stroke_points` 与 `brush`。
   */
  comp_tool_history_brush history;
  /*
   * 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   *
   * COMP_TOOL_HEAL 的 coverage 行数（宿主缓冲实际有多少行）。引擎按图层像素高
   * 逐行读 coverage；宿主若只分配了画布大小的缓冲、图层又比画布高，逐行读就会
   * 越过缓冲尾（域层在此之前只校验了 stride，看不到行数）。
   *
   * 0 = 旧宿主，按图层高信任（行为与追加前完全一致，既有测试不受影响）。
   * 非 0 且小于图层高 -> COMP_ERR_INVALID_ARGUMENT，宿主据此把「缓冲装不下」
   * 变成一次明确的拒绝，而不是未定义行为。
   */
  size_t           heal_coverage_rows;
  /*
   * P5-2 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   *
   * 仅 COMP_TOOL_TYPE 读取：可选字体句柄（comp_font_load_file / _load_memory 创建）。
   *   - NULL = 旧行为：内置 5×7 位图 ASCII 字体，文本只接受 0x20..0x7E 与 '\n'，
   *     `style.font_size` 是单元高；像素结果与 P5 之前逐字节一致；
   *   - 非 NULL = 用该字体排版：文本是 UTF-8（CJK 可用），`style.font_size` 变成
   *     **em 高**，行高与字距语义不变；字体里没有字形的字符只占步进、不绘制，
   *     note 会如实回报个数；句柄失效 -> COMP_ERR_STATE。
   *
   * 句柄只在本调用期间被读取，引擎不接管它的生命周期（仍由宿主 comp_font_release）。
   */
  comp_font        type_font;
  /*
   * P1-7 尾部追加（既有字段布局不变；宿主需用同一版本头文件编译）。
   *
   * 仅 COMP_TOOL_GRADIENT 读取：多色标与抖动开关。放在本结构体尾部而不是
   * `comp_tool_gradient` 里，是为了让 `comp_tool_op_desc` 里既有字段的偏移
   * 一个字节都不动。
   *   - `gradient_stops[0..gradient_stop_count)` 非空且 count >= 2 时，完全替代
   *     `gradient.color0/color1`；count == 0 或指针为 NULL 时走两色标旧路径
   *     （像素结果与追加前逐字节一致）；
   *   - count > COMP_TOOL_GRADIENT_MAX_STOPS 或指针为 NULL 而 count 非 0
   *     -> COMP_ERR_INVALID_ARGUMENT；
   *   - `gradient_dither` 非 0 = 对 RGB 加 ±0.5 LSB 的**确定性**抖动（同一坐标
   *     永远同一偏移，重复执行逐字节一致），用来压 8 位渐变带；0 = 旧行为。
   *
   * 色标缓冲只在本次调用期间被读取并拷贝进引擎，引擎不接管生命周期。
   */
  const comp_gradient_stop* gradient_stops;
  size_t                    gradient_stop_count;
  int32_t                   gradient_dither;
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

/* ------------------------------------------------- 名称 / 跨组移动（P0-1） */

/*
 * 读取图层名（UTF-8，含结尾 NUL）。
 *
 * 名字一直在文档里（`comp_layer_desc.name` 写入、`.comp` 原样落盘），此前
 * 唯一的读法是宿主持有自己那份 desc——撤销/重做、合并、读盘之后就对不上了。
 * 本入口是唯一的读出口：
 *   - `out_required` 写**所需字节数（含 NUL）**，即使 buffer 不够也照写；
 *   - `buffer == NULL` 或 `capacity == 0` 是**查询模式**：只回长度，返回 COMP_OK；
 *   - 缓冲区不够时**拒绝**（COMP_ERR_INVALID_ARGUMENT），不静默截断——截断
 *     后的 UTF-8 可能把一个汉字的三个字节切掉一个，宿主拿到的就是坏编码；
 *   - 只读：不开事务、不进撤销栈。
 */
COMP_API comp_result comp_layer_get_name(comp_document document,
                                         comp_layer_id id,
                                         char* buffer,
                                         size_t capacity,
                                         size_t* out_required);

/*
 * 设置图层名（UTF-8）。对齐上游图层面板的双击重命名：
 *   - name_utf8 必须非空、是严格合法的 UTF-8（拒绝超长编码、代理区与截断），
 *     且不超过 1024 字节；否则 COMP_ERR_INVALID_ARGUMENT，且不改旧名字；
 *   - 与其它属性写入一样**一步撤销**（加入调用方的显式事务，或自成一个隐式步）；
 *   - 名字不参与像素求值，因此不使合成缓存失效，宿主无需重绘画面。
 */
COMP_API comp_result comp_layer_set_name(comp_document document,
                                         comp_layer_id id,
                                         const char* name_utf8);

/*
 * 跨父级移动图层（图层面板把层拖进/拖出组）。
 *
 *   - `parent = 0` 表示根级，否则必须是已存在的**组**；
 *   - `before = 0` 表示移到目标父级的末尾；非 0 时必须与 parent 同级
 *     （与 comp_layer_reorder 同一把尺子，跨级锚点拒绝）；
 *   - 不许把组移进自己的子树（含 `parent == id`），否则出现祖先环；
 *   - 同级移动等价于 `comp_layer_reorder`（走同一条实现）；
 *   - 一步撤销；成功后堆叠序随新位置改变（数组序即堆叠序）。
 *
 * 与 comp_layer_reorder 的分工：后者**只**维持同父级次序，是既有的冻结语义；
 * 需要换父级时用本入口。错误码：图层/父级/锚点不存在 -> NOT_FOUND，
 * 父级不是组或锚点不同级 -> INVALID_ARGUMENT，成环 -> INVALID_ARGUMENT。
 */
COMP_API comp_result comp_layer_move(comp_document document,
                                     comp_layer_id id,
                                     comp_layer_id parent,
                                     comp_layer_id before);

/*
 * 复制图层（对齐上游 Duplicate Layer，⌘J / Ctrl+J）。
 *
 *   - 像素层：像素、蒙版、调整、效果与全部几何/混合属性深拷贝；
 *   - 组：整棵子树深拷贝（父子关系与组内剪贴引用按新 id 重映射）；
 *   - 副本落在原型之上、同级；名字追加 " copy"（按 UTF-8 边界截到 1024 字节内）；
 *   - 新的 layer id 与内容修订号都重新分配（副本是独立内容）；
 *   - 一步撤销，成功后 `out_id` 为副本 id。
 *
 * 错误码：图层不存在 -> NOT_FOUND；`out_id == NULL` -> INVALID_ARGUMENT。
 */
COMP_API comp_result comp_layer_duplicate(comp_document document,
                                          comp_layer_id id,
                                          comp_layer_id* out_id);

/* ------------------------------------------- 效果参数元数据（P0-2 尾部追加） */

/*
 * 已知效果类型（与 compositing/effect_eval.h 的 kEffectStroke..kEffectPatternOverlay
 * 及 shell ui_core 的 kEffectKindCount 数值一致）。
 *
 * 注意：这组枚举只用于**参数面板**。`comp_layer_set_effect` 的 effect 参数仍然
 * 不做白名单校验（未知 kind 原样落盘、不参与求值），本组接口对未知 kind 返回
 * COMP_ERR_INVALID_ARGUMENT —— 面板只编辑内核认识的 10 种，其它效果如实不提供编辑。
 */
typedef enum comp_effect_kind {
  COMP_EFFECT_STROKE = 0,
  COMP_EFFECT_DROP_SHADOW = 1,
  COMP_EFFECT_COLOR_OVERLAY = 2,
  COMP_EFFECT_INNER_SHADOW = 3,
  COMP_EFFECT_OUTER_GLOW = 4,
  COMP_EFFECT_INNER_GLOW = 5,
  /* P1-1 尾部追加。 */
  COMP_EFFECT_BEVEL = 6,
  COMP_EFFECT_SATIN = 7,
  COMP_EFFECT_GRADIENT_OVERLAY = 8,
  /* P1-1 第二段尾部追加。 */
  COMP_EFFECT_PATTERN_OVERLAY = 9,
} comp_effect_kind;

#define COMP_EFFECT_KIND_COUNT 10

/*
 * 单个可编辑效果参数的元数据。`json_path` 是 settings 对象的**顶层键**
 * （效果 settings 是扁平对象，如 "size" / "opacity"）；`default_value` 与
 * `effect_eval.cpp` 的 read_* 缺省值逐项一致，面板在键缺失时用它回填。
 *
 * settings 的写入侧仍是不透明字符串：本结构只描述「面板能编什么、量程多少」，
 * 不改变 `comp_layer_set_effect` 的冻结契约。`flags` 保留，恒为 0。
 */
typedef struct comp_effect_param {
  const char* json_path;
  const char* label;
  comp_param_type type;
  double min_value;
  double max_value;
  double default_value;
  uint32_t flags; /* 保留，恒为 0 */
} comp_effect_param;

/* 该 effect 的可编辑参数数量；effect 非法或 out_count 为 NULL -> INVALID_ARGUMENT。 */
COMP_API comp_result comp_layer_effect_param_count(uint32_t effect, size_t* out_count);

/* 取第 index 个参数元数据（index 越界 -> COMP_ERR_INVALID_ARGUMENT）。只读，不开事务。 */
COMP_API comp_result comp_layer_effect_param_at(uint32_t effect,
                                                size_t index,
                                                comp_effect_param* out_param);

/*
 * 从效果 settings JSON 按 json_path 读标量叶子，供面板回填既有值：
 * 命中数值写 out_number、命中布尔写 out_boolean（0/1），两个出参都可为 NULL。
 * 路径缺失、中间段非对象或叶子不是标量 -> COMP_ERR_NOT_FOUND（调用方回落到
 * param_at 的 default_value）。settings_json_utf8 为 NULL 按 "{}" 处理。
 */
COMP_API comp_result comp_layer_effect_read_value(const char* settings_json_utf8,
                                                  const char* json_path,
                                                  double* out_number,
                                                  int32_t* out_boolean);

/*
 * 由面板扁平值在**既有 settings** 上合并出新 settings（引擎侧组装，外壳不写 JSON）：
 *   - values[i] 对应 param_at(effect, i)，ASCII 文本（布尔 "0"/"1"），空串 = 该项不动；
 *     values 可少于 param_count（缺的按空串处理），多出的被忽略（与
 *     comp_layer_adjustment_build_settings 同口径）；
 *   - `base_settings_json_utf8` 是当前 settings（通常来自 comp_layer_effect_at）；
 *     未知键与未编辑的键**原样保留**——效果 settings 允许前向扩展，不能整棵重建；
 *   - 数值越界 / 非有限 / 文本非法 -> COMP_ERR_INVALID_ARGUMENT（不写半成品）；
 *   - 两段式：capacity 为 0 时只回报长度（含结尾 NUL 所需字节）；out_size 恒写
 *     正文长度（不含 NUL）。
 */
COMP_API comp_result comp_layer_effect_build_settings(uint32_t effect,
                                                      const char* const* values,
                                                      size_t value_count,
                                                      const char* base_settings_json_utf8,
                                                      char* settings_json_utf8,
                                                      size_t capacity,
                                                      size_t* out_size);

/* ------------------------------------------- 曲线控制点（P0-3 尾部追加） */

/*
 * Curves 的控制点读写（settings 里的 `curves.channels`，与上游 Curves.swift 同口径）。
 *
 * 通道下标有语义：0 = RGB 合成，1/2/3 = R/G/B（与 `comp_adjustment_kind` 的
 * COMP_ADJUSTMENT_CURVES 配合使用）。每通道 2..32 个点，x 严格递增、首点 x = 0、
 * 末点 x = 255，x/y 都在 0..255；求值是保形三次 Hermite，通道曲线先于 RGB 合成曲线。
 *
 * 这组入口与 `comp_layer_adjustment_*` 一样是**纯函数**：只在 settings JSON 上操作，
 * 不碰文档、不开事务、不进撤销栈——面板把返回的 JSON 交给
 * `comp_layer_update_adjustment_at` 才产生撤销步（配合显式事务即可做到「一次拖动一步」）。
 */
#define COMP_CURVE_CHANNEL_COUNT 4

/* 通道数（上下文无关，恒为 COMP_CURVE_CHANNEL_COUNT）；out_count 为 NULL -> INVALID_ARGUMENT。 */
COMP_API comp_result comp_layer_curves_channel_count(size_t* out_count);

/*
 * 某通道当前控制点数量（按 x 升序、同 x 去重后的条数）。
 * settings 非法、通道缺失或数组为空 -> 0（语义即恒等曲线），仍返回 COMP_OK；
 * channel 越界或 out_count 为 NULL -> COMP_ERR_INVALID_ARGUMENT。
 */
COMP_API comp_result comp_layer_curves_point_count(const char* settings_json_utf8,
                                                   uint32_t channel,
                                                   size_t* out_count);

/*
 * 读第 index 个控制点；index 越界 -> COMP_ERR_NOT_FOUND（调用方回落到恒等）。
 * 与 point_count 用同一份排序去重结果，两个入口不会漂移。
 */
COMP_API comp_result comp_layer_curves_point_at(const char* settings_json_utf8,
                                                uint32_t channel,
                                                size_t index,
                                                double* out_x,
                                                double* out_y);

/*
 * 写某通道控制点，保留 settings 里其余键（其他通道、`curves.channel` 等）：
 *   - `points_xy` 是 `point_count` 个 (x, y) 对（共 2*point_count 个 double），
 *     顺序任意，引擎排序后校验；
 *   - `point_count == 0` 表示复位成恒等曲线 (0,0)-(255,255)，`points_xy` 可为 NULL；
 *   - 点数不在 2..32、x 重复、首点 x != 0、末点 x != 255、x/y 越界或非有限 ->
 *     COMP_ERR_INVALID_ARGUMENT（不写半成品）；
 *   - 两段式：capacity 为 0 时只回报长度（正文长度写 out_size，不含 NUL），
 *     容量不够则截断并补 NUL（与 adjustment_build_settings 同口径）。
 */
COMP_API comp_result comp_layer_curves_set_points(const char* settings_json_utf8,
                                                  uint32_t channel,
                                                  const double* points_xy,
                                                  size_t point_count,
                                                  char* out_json_utf8,
                                                  size_t capacity,
                                                  size_t* out_size);

/* ------------------------------------------- 像素回读（P0-4 尾部追加） */

/*
 * 读取图层像素的 **8 位工作副本**（预乘 RGBA8、4 字节/像素、行优先、自上而下，
 * 尺寸为该图层像素矩形，**不是**画布尺寸；画布落点由 comp_layer_get_transform
 * 给出）。这是 `comp_layer_set_pixels` 的读侧对偶，补它是为了让宿主能做
 * 「读—改—写」：剪贴板复制/剪切、删除选区内像素都要求先看到图层当前内容。
 *
 * 与 `comp_render_layer_thumbnail` 的分工：那个返回**画布坐标下看到的单层合成**
 * （transform、蒙版、效果、透明度全部烘进去，用于缩略图）；本入口返回**图层
 * 原始资产**，不做任何合成。需要「用户看到的那一层」用前者，需要「把像素改掉
 * 再写回」用本入口。
 *
 * 16 位 / HDR 图层先走既有的 8 位工作副本（`deep_to_rgba8` / `hdr_to_rgba8`，
 * 与合成、缩略图、工具层同一份口径），`out_format` 仍回报**原始**格式：宿主
 * 据此判断能否无损写回——RGBA16 / RGBA16F 经 `comp_layer_set_pixels` 回写会
 * 降为 8 位（见该函数契约），宁可拒绝也不要静默降位。
 *
 * 契约：
 *   - **两段式**：`pixels == nullptr`（或 `stride == 0`）是查询模式，只写
 *     `out_width / out_height / out_format` 并返回 COMP_OK——宿主必须先问尺寸
 *     再分配，否则「按画布尺寸分配、图层其实更大」会越界写；
 *   - 给了缓冲时 `stride` 必须 >= width * 4（不足 -> COMP_ERR_INVALID_ARGUMENT，
 *     不写半行）；缓冲至少 `height * stride` 字节，由调用方保证；
 *   - 空图层（组、或没有像素资产的层）写 `out_width = out_height = 0` 并返回
 *     COMP_OK —— 「没有像素」是合法状态，不是错误。注意新建的**像素层**默认
 *     铺满画布（全透明），所以它报的是画布尺寸而不是 0；
 *   - 图层不存在 -> COMP_ERR_NOT_FOUND；
 *   - **只读**：不开事务、不进撤销栈、不改内容修订号（不影响合成缓存）。
 *
 * ABI 尾部追加：前序函数与结构体布局不变（145 -> 146）。
 */
COMP_API comp_result comp_layer_get_pixels(comp_document document,
                                           comp_layer_id id,
                                           void* pixels,
                                           size_t stride,
                                           int32_t* out_width,
                                           int32_t* out_height,
                                           comp_pixel_format* out_format);

/* ------------------------------------------- 可编辑文字图层（P0-6 ① 尾部追加） */

/* 段落对齐方式（P0-6 ③ 尾部追加）：0 = 左 / 1 = 居中 / 2 = 右。
 * 以 comp_text_desc.box_width 为基准；框宽为 0 时三种落点相同。 */
typedef enum comp_text_align {
  COMP_TEXT_ALIGN_LEFT = 0,
  COMP_TEXT_ALIGN_CENTER = 1,
  COMP_TEXT_ALIGN_RIGHT = 2,
  COMP_TEXT_ALIGN_COUNT = 3
} comp_text_align;

/*
 * 文字图层描述（语义见 domain/text_object.h）。
 *
 * 与 `comp_tool_type` 的区别是**语义**上的，不是字段上的：`comp_tool_type` 把文字
 * 一次性画进当前图层的像素（画完就不可再编辑）；本结构描述一个**持久的文字
 * 对象**，引擎把它挂到图层上、渲染结果作为该层像素的缓存——改文本/字号/颜色
 * 只需重新渲染这一层，其它图层逐字节不变，撤销/重做与 `.comp` 往返都把对象
 * 原样带回来。
 *
 * 两个轨道（与 comp_tool_type 完全一致）：
 *   - `font_path_utf8` 为 NULL/空串：内置 5×7 位图 ASCII，文本只接受
 *     0x20..0x7E 与 '\n'，`style.font_size` 是 5×7 单元高；
 *   - 非空：从该路径加载（进程级缓存，见 domain/text_layer.h），文本按 UTF-8
 *     解码、CJK 可用，`style.font_size` 是 **em 高**。字体打不开 -> COMP_ERR_IO /
 *     COMP_ERR_DECODE（不静默回落成内置点阵，那会让宿主以为字体生效了）。
 *
 * 为什么存路径而不是 comp_font 句柄：文字对象要进 `.comp`，句柄在存盘后就失效；
 * 路径 + `font_face_index` 是唯一能跨会话还原的标识。`font_family` 只用于回显，
 * 不参与渲染（族名在不同机器上可能指向不同文件）。
 *
 * 几何：`origin_x/origin_y` 是文本框**左上角**（文档像素，可带小数，亚像素偏移
 * 会保留在字形落位上）。写入成功后图层的 `transform` 被设为文本框矩形（整数
 * 像素，原点取 floor），因此移动文字图层 = 移动 transform = 移动文本框。
 *
 * 参数闸（COMP_ERR_INVALID_ARGUMENT，且不改旧像素、不进撤销栈）：文本为空 /
 * 超过 65536 字节 / 旧轨道含非可打印 ASCII（含 UTF-8、Tab）/ 字号、行距、字距
 * 越界或非有限 / 原点非有限或绝对值 > 1e6 / 颜色 alpha 为 0 / opacity 不在
 * (0, 1] / `_reserved` 非 0。文本框与画布完全不相交 -> COMP_ERR_INVALID_ARGUMENT
 * （不会建出一张看不见的层）。文本框面积超过画布上限同量级 -> COMP_ERR_UNSUPPORTED。
 *
 * 段落框（P0-6 ③，**尾部追加**，布局不变）：`box_width == 0` 时是「点文字」，
 * 按内容自动定宽（与 P0-6 之前逐字节一致）；`box_width > 0` 时按框宽贪心折行，
 * 断点取空格之后或宽字符（CJK/全角/Emoji）边界，一整段不可断的词比框还宽时
 * 按字符硬断。`box_height > 0` 只负责裁切（超出框底的行不画），不参与折行。
 * `align` 以框宽为基准（没有框宽时三种对齐落点相同）。两个框值都按**文档像素**
 * 计，量程 [0, 1e6]；框宽/框高只声明其中一维时，另一维仍按内容尺寸。
 */
typedef struct comp_text_desc {
  const char* text;        /* UTF-8；'\n' 断行；上级轨道仅 0x20..0x7E */
  int32_t text_length;     /* 字节数；1..65536 */
  double origin_x;         /* 文本框左上角，文档像素 */
  double origin_y;
  comp_type_style style;
  uint8_t color[4];        /* 直通 sRGB + alpha（0..255） */
  float opacity;           /* (0, 1]，再乘进逐像素覆盖率 */
  const char* font_path_utf8; /* NULL/空 = 内置 5×7 点阵 ASCII */
  int32_t font_face_index; /* .ttc 面序号，0 = 第一面；无字体时忽略 */
  int32_t _reserved;       /* 必须为 0 */
  /* ---- P0-6 ③ 尾部追加：段落框 ---- */
  double box_width;        /* 0 = 按内容自动（点文字）；> 0 = 折行框宽 */
  double box_height;       /* 0 = 不裁切；> 0 = 框高，超出部分不落墨 */
  int32_t align;           /* comp_text_align；以 box_width 为基准 */
  int32_t _reserved2;      /* 必须为 0 */
} comp_text_desc;

/*
 * 新建文字图层（对齐上游「用文字工具点一下建一个可再编辑的文字层」）。
 *
 *   - `desc` / `out_id` 为 NULL -> COMP_ERR_INVALID_ARGUMENT；
 *   - 参数与字体错误在**开事务之前**拒绝：不建层、不消耗图层 id、不留半成品；
 *   - 图层名默认取文本第一行前 32 字节（UTF-8 边界安全），空行回落「文字图层」；
 *   - 新层放在当前活动图层的**上方**（与 comp_layer_add 同一套堆叠语义）；
 *   - 全局不透明度为 1、混合 Normal、采样 High quality；颜色/透明度由 desc 决定；
 *   - 一步撤销；返回的 id 可直接用于 comp_layer_set_text / comp_layer_move。
 */
COMP_API comp_result comp_layer_add_text(comp_document document,
                                         const comp_text_desc* desc,
                                         comp_layer_id* out_id);

/*
 * 改写已有文字图层的文字对象并重新渲染它的像素。
 *
 *   - `id` 不是文字图层 -> COMP_ERR_STATE（提示用 comp_layer_add_text 新建，
 *     或先 rasterize）；**不会**悄悄把普通图层的像素换成文字；
 *   - 参数闸同 comp_layer_add_text，失败时旧文本/旧像素原样保留；
 *   - 成功后图层 `transform` 跟随新文本框、内容修订号推进（合成缓存/GPU 纹理
 *     自动失效），一步撤销——连续拖动字号滑块时宿主应当用 comp_txn_* 把这些
 *     调用合并成一个撤销步；
 *   - 调整/效果/蒙版/混合模式不属于文字对象，改文字时原样保留。
 */
COMP_API comp_result comp_layer_set_text(comp_document document,
                                         comp_layer_id id,
                                         const comp_text_desc* desc);

/*
 * 读回文字图层。
 *
 *   - 不是文字图层 -> COMP_ERR_STATE；图层不存在 -> COMP_ERR_NOT_FOUND；
 *   - `out_desc` 非空时整块写入（`text` 字段被改写成 `text_buffer`，可为 NULL）；
 *   - 文本两段式：`text_buffer == NULL` 或 `capacity == 0` 只回填长度；
 *     `*out_required` = 文本字节数 **+ 1**（含结尾 NUL，与 comp_layer_get_name
 *     同一把尺子）；容量不足返回 COMP_ERR_INVALID_ARGUMENT，不写半截字符串；
 *   - 只读：不开事务、不推进修订号、不影响合成缓存。
 */
COMP_API comp_result comp_layer_get_text(comp_document document,
                                         comp_layer_id id,
                                         comp_text_desc* out_desc,
                                         char* text_buffer,
                                         size_t capacity,
                                         size_t* out_required);

/*
 * 把文字图层栅格化：丢掉文字对象，像素保留为普通图层。
 *
 *   - 这是「在文字上落笔」「抠图/滤镜改像素」之前的显式闸门：栅格化之后该层
 *     就再也不能改字号/文本（与 Photoshop 的「栅格化文字」同义）；
 *   - 不是文字图层 -> COMP_OK 且不改任何东西（幂等，宿主不必先判断）；
 *   - 一步撤销（撤销后文字对象与可编辑性一起回来）；
 *   - 图像大小重采样（comp_document_resize_image）会隐式丢弃文字对象：
 *     非等比缩放无法用「字号 + 文本框」表达，宁可退回普通图层也不撒谎。
 */
COMP_API comp_result comp_layer_rasterize_text(comp_document document, comp_layer_id id);

/* ------------------------------------------- 图层锁 / 颜色标签（P1-2 ① 尾部追加） */

/*
 * 图层锁位掩码。位可以组合（例如「锁定透明 + 锁定位置」），`COMP_LAYER_LOCK_ALL`
 * 是「全部锁定」的显式位：判定时它**蕴含**另外三位，不需要调用方再或进去。
 *
 * 语义（与 Photoshop 的四个锁按钮一一对应）：
 *   - `TRANSPARENT`：只允许改已有像素的颜色，alpha 通道被保住。落在全透明处的
 *     笔迹被丢弃；已有像素的 alpha 原样保留，新颜色按原 alpha 重新预乘。
 *     只管**绘画类工具**（画笔/橡皮/填充/渐变/涂抹/修饰等，即 `comp_tool_apply`
 *     里除 SELECT 与 DEEP 之外的算子）；`comp_tool_select` 写的是蒙版、`comp_tool`
 *     的 DEEP 系列换的是像素格式，两者都不受透明锁约束（受 `ALL` 约束的是前者）。
 *     滤镜/反相/Camera Raw 不走事后 alpha 还原，所以也不受它约束（要拦它们用
 *     `PIXELS`）。位深限制：本轮只在 RGBA8 上强制；RGBA16 / RGBA16F 图层带透明
 *     锁调用工具会得到 COMP_ERR_STATE（16 位与 half 的 alpha 需要按各自编码缩放，
 *     那是后续批次的事），不会静默忽略锁。
 *   - `PIXELS`：禁止任何会改变图层像素的入口（工具落墨、滤镜、反相、破坏性调整、
 *     Camera Raw、整层像素写入、文字重排版）。改锁、改混合/不透明度、改位置不受影响。
 *   - `POSITION`：禁止几何变换——`comp_layer_set_transform` 与 `comp_layer_set_flip`。
 *     图层面板里的**堆叠次序**不算位置：`comp_layer_move` 是树上的重挂（换父组/层序），
 *     不是空间移动，锁位置后重排仍允许（PS 同口径：锁位置后仍可在面板里拖动层序）。
 *   - `ALL`：上面三项全含，另外拒绝删除、向下合并、栅格化文字，以及调整层/效果
 *     的增删改。
 *
 * 不参与合成的属性：锁与颜色标签都是**文档状态**，随撤销/重做回退、随 `.comp`
 * 往返，但合成、缩略图、导出、合并、拼合一律不看锁——锁只会让「写」失败，
 * 不会让画面悄悄变样。
 */
typedef enum comp_layer_lock {
  COMP_LAYER_LOCK_NONE = 0,
  COMP_LAYER_LOCK_TRANSPARENT = 1u << 0,
  COMP_LAYER_LOCK_PIXELS = 1u << 1,
  COMP_LAYER_LOCK_POSITION = 1u << 2,
  COMP_LAYER_LOCK_ALL = 1u << 3
} comp_layer_lock;

/* 允许出现在 lock_flags 里的全部位（其余位一律拒绝，不静默丢弃）。 */
#define COMP_LAYER_LOCK_MASK 0x0Fu

/*
 * 设置图层锁（整份覆盖，不是「或」进去）：`flags` 只接受
 * `COMP_LAYER_LOCK_MASK` 内的位，其它位 -> COMP_ERR_INVALID_ARGUMENT 且不改旧值
 * ——保留未知位会让宿主以为锁生效了，实际却没有对应语义。
 *
 * 一步撤销（加入调用方的显式事务，或自成一个隐式步）；与 set_visible 同一口径，
 * 不推进内容修订号、不使合成缓存失效（锁不参与像素求值）。图层不存在 ->
 * COMP_ERR_NOT_FOUND。
 */
COMP_API comp_result comp_layer_set_lock(comp_document document, comp_layer_id id,
                                         uint32_t flags);

/* 读回图层锁；与 set_lock 成对，供面板勾选态与置灰回读。只读，不开事务。 */
COMP_API comp_result comp_layer_get_lock(comp_document document, comp_layer_id id,
                                         uint32_t* out_flags);

/*
 * 颜色标签取值。0 = 无标签；1..7 的顺序与 Photoshop 的面板色板一致
 * （红 / 橙 / 黄 / 绿 / 蓝 / 紫 / 灰），只是**语义标签**，不参与合成。
 */
typedef enum comp_layer_color_label {
  COMP_LAYER_COLOR_NONE = 0,
  COMP_LAYER_COLOR_RED = 1,
  COMP_LAYER_COLOR_ORANGE = 2,
  COMP_LAYER_COLOR_YELLOW = 3,
  COMP_LAYER_COLOR_GREEN = 4,
  COMP_LAYER_COLOR_BLUE = 5,
  COMP_LAYER_COLOR_VIOLET = 6,
  COMP_LAYER_COLOR_GRAY = 7
} comp_layer_color_label;

#define COMP_LAYER_COLOR_COUNT 8

/*
 * 设置颜色标签。`label` 必须在 0..7（COMP_LAYER_COLOR_COUNT-1），越界 ->
 * COMP_ERR_INVALID_ARGUMENT 且不改旧值。一步撤销；不推进修订号（纯元数据）。
 * 图层不存在 -> COMP_ERR_NOT_FOUND。
 */
COMP_API comp_result comp_layer_set_color_label(comp_document document, comp_layer_id id,
                                                uint32_t label);

/* 读回颜色标签（0..7）。只读，不开事务。 */
COMP_API comp_result comp_layer_get_color_label(comp_document document, comp_layer_id id,
                                                uint32_t* out_label);

/* ------------------------------------------- 合并可见 / 反向排列（P1-2 ② 尾部追加） */

/*
 * 合并可见（对齐 Photoshop 的 Merge Visible；PS 默认 Ctrl+Shift+E，本壳该键已被
 * 「效果参数」占用，Windows 壳改绑 Ctrl+Alt+E、命令号 1673）：把当前文档里所有
 * **实际参与合成**的图层（自身可见、且祖先组全部可见）合成一个像素层，落在
 * 最高的那个参与单元（根级图层或根级组）的槽位；隐藏图层与隐藏组原样保留。
 *
 * 语义与边界：
 *   - 可见组按**整体**烘焙（组内可见子层由合成器处理）；组内的**隐藏**后代
 *     不参与合并，组消失后被提升到组原来的父级——它们不可见，所以渲染不变；
 *   - 参与层剪贴到的基底若**不参与**合并（例如可见层剪贴到隐藏层）-> 明确拒绝：
 *     剪贴因数只看基底像素、不看可见性，剪断剪贴会改变画布；
 *   - 携带引擎不认识的 effect -> 明确拒绝（烘焙会静默丢弃，与 merge_down 同口径）；
 *   - 实际贡献像素的图层少于 2 个 -> 空操作：返回 COMP_OK、`*out_changed = 0`，
 *     不进历史；
 *   - 一步撤销（加入调用方的显式事务，或自成一个隐式步）。
 *
 * `out_layer_id` 可空；非空时写回结果层 id（空操作时写 0）。`out_changed` 可空。
 */
COMP_API comp_result comp_layer_merge_visible(comp_document document, comp_layer_id* out_layer_id,
                                              int32_t* out_changed);

/*
 * 反向排列（对齐 Photoshop 的 Layer > Arrange > Reverse）：反转 `parent` 下的
 * **直接**兄弟顺序。`parent == 0` 表示根级；非 0 必须存在且是图层组，否则
 * COMP_ERR_NOT_FOUND / COMP_ERR_STATE。
 *
 * 只重排，不改像素、几何或混合；兄弟少于 2 个时空操作（COMP_OK、
 * `*out_changed = 0`，不进历史）。一步撤销。`out_changed` 可空。
 */
COMP_API comp_result comp_layer_reverse(comp_document document, comp_layer_id parent,
                                        int32_t* out_changed);

/* ------------------------------------------------- 图层链接（P1-2 ③ 尾部追加） */

/*
 * 图层链接（对齐 Photoshop 的 Link Layers）：`count` 个图层并成同一个链接组，
 * 之后任一层的**几何变换**（位移/缩放/旋转，统一走 comp_layer_set_transform）
 * 会以同样的仿射量作用到同组其它层。
 *
 * 语义与边界：
 *   - `count < 2` / `ids == NULL` / 有重复 id -> COMP_ERR_INVALID_ARGUMENT；
 *   - 任一层不存在 -> COMP_ERR_NOT_FOUND；
 *   - 含图层组 -> COMP_ERR_UNSUPPORTED（组的变换是壳层的几何，不做联动）；
 *   - 已经在同一非空组 -> 空操作（COMP_OK、`*out_changed = 0`，不进历史）；
 *   - ids 分属不同既有组时**合并**这些组（原组成员一并改投新组，不把旧组拆半）；
 *     全未链接时分配新组号（现有最大组号 + 1，不复用空洞）；
 *   - 链接是纯关系元数据：不改像素/几何、不推进修订号、不使合成缓存失效；
 *   - 一步撤销（加入调用方的显式事务，或自成一个隐式步）。
 *
 * `out_group` / `out_changed` 可空；`out_group` 写回组号（>0；空操作时写既有组号）。
 */
COMP_API comp_result comp_layer_link(comp_document document, const comp_layer_id* ids,
                                     size_t count, uint32_t* out_group, int32_t* out_changed);

/*
 * 解除链接：把 `ids` 中图层的链接组清 0（同组未被列举的层保持原组，PS 同款：
 * 解除只作用于「被选中的层」）。
 *
 * `count == 0` / `ids == NULL` / 重复 id -> COMP_ERR_INVALID_ARGUMENT；任一层
 * 不存在 -> COMP_ERR_NOT_FOUND；都不在组 -> 空操作（COMP_OK、`*out_changed = 0`）。
 * 一步撤销；纯元数据，不碰修订号与合成缓存。
 */
COMP_API comp_result comp_layer_unlink(comp_document document, const comp_layer_id* ids,
                                       size_t count, int32_t* out_changed);

/*
 * 读回链接组号（0 = 未链接）。只读，不开事务。图层不存在 -> COMP_ERR_NOT_FOUND。
 */
COMP_API comp_result comp_layer_get_link(comp_document document, comp_layer_id id,
                                         uint32_t* out_group);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_LAYER_H */

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

/* 替换图层像素内容。pixels 可为 NULL 表示清空为全透明。 */
COMP_API comp_result comp_layer_set_pixels(comp_document document,
                                           comp_layer_id id,
                                           const comp_image* image);

/* 设置/清除图层蒙版；mask 为 NULL 表示移除蒙版。 */
COMP_API comp_result comp_layer_set_mask(comp_document document,
                                         comp_layer_id id,
                                         const comp_mask* mask);

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_LAYER_H */

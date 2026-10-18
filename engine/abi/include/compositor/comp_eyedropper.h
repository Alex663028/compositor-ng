// AI生成
/*
 * comp_eyedropper.h — 吸管取色 C ABI（S5）。
 *
 * 语义唯一真源见 domain/eyedropper.h 的文件头：
 *   - 坐标为文档像素（左上原点，允许小数）；
 *   - 输出是**非预乘** sRGB 0..1（RGBA），可直接作为画笔颜色；
 *   - sample_size 是方形窗口边长（奇数 1..101），按预乘值平均后再反预乘；
 *   - source 0 = 目标图层，1 = 合并结果；
 *   - 16 位 / HDR 自动走 8 位工作副本，取到的是屏幕上的颜色。
 *
 * 注意：取色是**只读**操作，不开事务、不产生撤销步。
 */
#ifndef COMPOSITOR_COMP_EYEDROPPER_H
#define COMPOSITOR_COMP_EYEDROPPER_H

#include "comp_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum comp_eyedropper_source {
  COMP_EYEDROPPER_LAYER  = 0, /* 只从 target 图层取样 */
  COMP_EYEDROPPER_MERGED = 1  /* 从整篇合成取样（target 被忽略） */
} comp_eyedropper_source;

/*
 * 在 (x, y) 处取色，写入 out_srgb[4]（直通 sRGB，0..1）。
 *
 * 错误码：
 *   out_srgb 为 NULL / 坐标非有限 / 采样尺寸非奇数或越界 / source 未知
 *     -> COMP_ERR_INVALID_ARGUMENT
 *   坐标落在画布外                       -> COMP_ERR_INVALID_ARGUMENT
 *   图层不存在                           -> COMP_ERR_NOT_FOUND
 *   组图层 / 无像素图层                   -> COMP_ERR_STATE
 *   合成或 8 位副本失败                   -> COMP_ERR_INTERNAL
 */
COMP_API comp_result comp_eyedropper_sample(comp_document document, comp_layer_id layer, double x,
                                            double y, int32_t sample_size,
                                            comp_eyedropper_source source, float out_srgb[4]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_EYEDROPPER_H */

// AI生成
/*
 * comp_canvas.h —「图像」菜单的画布级操作（S25）。
 *
 * 三个入口，语义唯一真源是 Photoshop 的 Image 菜单与上游 Compositor：
 *   comp_document_rotate_canvas  Image > Image Rotation（90° CW / CCW / 180°）
 *                                ← 上游 CanvasRotation.swift（quarterTurned）
 *   comp_document_trim           Image > Trim
 *                                ← 上游 ImageTrim.swift
 *   comp_layer_invert            Image > Adjustments > Invert（破坏性一次性）
 *                                ← 上游 PixelInvert.swift
 *
 * 贯穿全篇的口径（与 comp_crop_* 一致）：
 *   1. 坐标是**文档像素**、左上原点；
 *   2. 旋转与修边都**不重采样**：只平移图层 origin / 换画布尺寸，图层像素
 *      一个字节不动；画布外内容仍在，再裁回来还会露出来；
 *   3. 写操作在未处于显式事务时自动补一个隐式事务：整次操作恰好一步撤销；
 *   4. 无变化不进历史（out_changed = 0）。
 *
 * 与调整层的分工：COMP_ADJUSTMENT_INVERT 是非破坏可编辑调整层；
 * comp_layer_invert 是破坏性一次性落墨——上游两者并存，本仓也对齐。
 */
#ifndef COMPOSITOR_COMP_CANVAS_H
#define COMPOSITOR_COMP_CANVAS_H

#include "comp_core.h"
#include "comp_layer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 画布旋转 */

/*
 * 旋转整个画布（PS Image Rotation；上游 quarterTurned）。
 *
 * quarter_turns：1 = 顺时针 90°，-1 = 逆时针 90°，2 / -2 = 180°。
 * 其它值（含 0）返回 COMP_ERR_INVALID_ARGUMENT，文档不变。
 *
 * 90° 时画布 width/height 互换（180° 不变）。所有图层、蒙版、效果、调整、
 * 组关系与文档级状态随行：**不重采样**——图层像素一个字节不动，落位
 * transform 换成旋转后的轴对齐包围盒（90° 时 w/h 互换）+ rotation 累加。
 * 选区是文档分辨率蒙版，按 90°/180° 精确转置随动。
 *
 * 上游 position 是 Double，本仓 transform origin 是 int32_t：奇数边长旋转
 * 后按「.5 远离零」取整，可能丢半像素（不重采样原则下无可选）。
 *
 * 图层 origin 越出引擎上限（±1e6）时返回 COMP_ERR_INVALID_ARGUMENT，
 * **不改任何状态**（同 comp_crop_apply 的两遍校验口径）。
 *
 * 唯一 out_changed = 0 的情形：空文档（无图层、无选区）且旋转后画布尺寸
 * 不变（180° 或正方形画布的 90°）——状态逐字节相同，不进历史。
 *
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_document_rotate_canvas(comp_document document, int32_t quarter_turns,
                                                 int32_t* out_changed);

/* ------------------------------------------------------------ 修边 Trim */

/* 修边基准（上游 TrimBasedOn rawValue，数值一一对应）： */
typedef enum comp_trim_based_on {
  COMP_TRIM_TRANSPARENT_PIXELS = 0,   /* "Transparent Pixels" */
  COMP_TRIM_TOP_LEFT_PIXEL_COLOR = 1, /* "Top Left Pixel Color" */
  COMP_TRIM_BOTTOM_RIGHT_PIXEL_COLOR = 2, /* "Bottom Right Pixel Color" */
} comp_trim_based_on;

/*
 * 修边选项（上游 TrimOptions）。
 *
 * tolerance 是**逐通道**容差（0..255）：颜色基准模式下 RGBA 四通道的绝对
 * 差都 <= tolerance 才算「与基准色相同」被裁掉（上游 calculateColorTrimRect
 * 的 abs(...) <= tol 同口径）。top / bottom / left / right 关闭的边不裁
 * （保持 0 / width / height）。四边全关在 apply 时返回
 * COMP_ERR_INVALID_ARGUMENT（上游 trimsAny guard）。
 */
typedef struct comp_trim_options {
  comp_trim_based_on based_on;
  int32_t top;      /* != 0 时裁掉顶部多余行 */
  int32_t bottom;
  int32_t left;
  int32_t right;
  uint8_t tolerance;
} comp_trim_options;

/*
 * 提交修边（PS Image > Trim；上游 calculateTrimRect + 裁剪提交）。
 *
 * 先 flatten 整篇**可见合成**计算边框（不看选区，与 PS 一致），再按
 * comp_crop_apply 的「平移图层 + 换画布」提交，一步撤销。
 *
 * - based_on 枚举未知、四边全关 → COMP_ERR_INVALID_ARGUMENT
 *   （tolerance 是 uint8_t，天然落在 0..255，不再单独判上限）；
 * - 空文档（无图层）→ COMP_ERR_STATE；
 * - 整篇没有可保留内容（全透明 / 整图同基准色）→ COMP_ERR_STATE
 *   （上游 noContentToTrim），文档不变；
 * - 修出边框后由 crop 口径处理：与当前画布相同 → out_changed = 0；
 *   图层 origin 越界 → COMP_ERR_INVALID_ARGUMENT，文档不变；
 * - **活动选区会被清掉**（换画布后旧坐标失效，与 comp_crop_apply 同口径；
 *   这一步在事务内，撤销会把选区一起还原）。
 *
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_document_trim(comp_document document, comp_trim_options options,
                                        int32_t* out_changed);

/* ------------------------------------------------------------ 像素反相 */

/*
 * 反相目标图层的像素（PS Image > Adjustments > Invert；上游 PixelInvert）。
 *
 * 预乘 RGBA 语义（上游 PixelInvert 注释）：每条颜色通道 c → alpha - c，
 * alpha 保持不变——透明像素反相后仍透明，不会变实心。
 *
 * - 活动选区存在时结果按 coverage 与原像素混合（选区按文档坐标解释，
 *   与 comp_filter_apply 同一条 blend 口径；图层的 rotation / flip 参与
 *   文档坐标换算，旋转后的图层不会把选区套错位置）；显式空选区 = 什么都不改，
 *   out_changed = 0；
 * - 16 位图层先落 8 位工作副本（同 comp_filter_apply），**写回后该图层永久
 *   变成 RGBA8**（深度是有损的，撤销可恢复原 16 位快照）；HDR（RGBA16F）
 *   图层返回 COMP_ERR_UNSUPPORTED；
 * - 图层不存在 / 是组 / 无像素 → COMP_ERR_NOT_FOUND / COMP_ERR_STATE；
 * - 一步撤销（显式事务内加入，否则自成隐式步）。
 *
 * out_changed 可为 NULL。
 */
COMP_API comp_result comp_layer_invert(comp_document document, comp_layer_id layer,
                                       int32_t* out_changed);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* COMPOSITOR_COMP_CANVAS_H */

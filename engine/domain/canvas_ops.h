// AI生成
/*
 * canvas_ops.h —「图像」菜单的一次性整篇/整层操作（S25）。
 *
 * 语义唯一真源是上游 Photoshop 工作流里 Image 菜单下的三条通路：
 *   - Image > Image Rotation > 90° CW / 90° CCW / 180°
 *     ← 上游 `Compositor/Document/CanvasRotation.swift`（quarterTurned）
 *   - Image > Trim
 *     ← 上游 `Compositor/Document/ImageTrim.swift`（calculateTrimRect + 裁剪提交）
 *   - Image > Adjustments > Invert（破坏性一次性，区别于调整层 INVERT）
 *     ← 上游 `Compositor/Document/PixelInvert.swift`
 *
 * 与裁剪（crop.h）共用同一条「不重采样」口径：旋转与修边都只平移图层
 * origin / 换画布尺寸，图层像素一个字节都不动；画布外的内容仍在，再裁
 * 回来还会露出来。这与上游 CanvasResizer / quarterTurned 完全一致。
 *
 * 三条口径（与 filters / tool_ops / camera_raw 一致）：
 *   1. 写操作一步撤销：加入调用方已开启的显式事务，或自成一个隐式步；
 *   2. 参数闸在开事务之前：非法参数不写半成品、不进撤销栈；
 *   3. 无变化不进历史（out_changed = 0）。
 *
 * 与调整层的分工：COMP_ADJUSTMENT_INVERT 是非破坏的可编辑调整层，
 * 这里的 invert_layer 是破坏性的一次性像素操作（上游两者都有）。
 */
#ifndef COMPOSITOR_DOMAIN_CANVAS_OPS_H
#define COMPOSITOR_DOMAIN_CANVAS_OPS_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "compositor/comp_core.h"
#include "compositor/comp_layer.h"

#include "domain/domain_state.h"

namespace compositor {

class Document;

/* ------------------------------------------------------------ 画布旋转 */

/*
 * 旋转整个画布（上游 quarterTurned；PS Image Rotation）。
 *
 * quarter_turns ∈ {1, -1, 2, -2}：1 = 顺时针 90°，-1 = 逆时针 90°，
 * 2 与 -2 均为 180°。其它值（含 0）返回 false。
 *
 * 90° 时画布 width/height 互换；180° 时不变。每个图层的中心点按上游
 * quarterTurned 的映射转到新画布，rotation 累加并归一化到 (-360, 360]。
 * **不重采样**：图层像素一个字节不动；落位 transform 换成旋转后的轴对齐
 * 包围盒（90° 时 w/h 互换）——渲染器的 rotation 绕 bounds 中心转、光栅
 * 窗口取轴对齐 bounds，不换包围盒会让旋转出去的一半内容被裁空。
 *
 * 图层数据（像素 / 蒙版 / 效果 / 调整 / 组关系）全部随行：蒙版是图层
 * 本地坐标（与上游 mask 归一化范围一致），图层几何变了它自然跟着走。
 * **选区**是文档分辨率蒙版，按 90°/180° 精确转置（无插值损失）。
 *
 * 上游 position 是 Double；本仓 transform origin 是 int32_t，奇数边长
 * 旋转后按 llround（.5 远离零）取整，可能丢半像素——如实记录，不重采样
 * 原则下这是唯一可选项。
 *
 * `changed = false` 的唯一情形：文档完全空（无图层、无选区）且旋转后
 * 画布尺寸不变（180° 或正方形 90°）——此时状态逐字节相同，不进历史。
 */
bool rotate_canvas(DocumentState* state, int32_t quarter_turns, bool* changed);

/* ------------------------------------------------------------ 修边 Trim */

/* 基准色：与上游 TrimBasedOn 的 rawValue 一一对应。 */
enum class TrimBasedOn : int32_t {
  TransparentPixels = 0,   /* "Transparent Pixels"：按 alpha 边界 */
  TopLeftPixelColor = 1,   /* "Top Left Pixel Color" */
  BottomRightPixelColor = 2, /* "Bottom Right Pixel Color" */
};

/*
 * 修边选项（上游 TrimOptions）。tolerance 是**逐通道**容差（与上游
 * calculateColorTrimRect 的 abs(...) <= tol 同口径，RGBA 四通道都比）。
 * top/bottom/left/right 关闭的边保持 0/width/height（上游同款）。
 */
struct TrimOptions {
  TrimBasedOn based_on = TrimBasedOn::TransparentPixels;
  bool top = true;
  bool bottom = true;
  bool left = true;
  bool right = true;
  uint8_t tolerance = 0;
};

/*
 * 纯函数：从**整篇合成**的 RGBA8（预乘与否不限，只读 alpha / RGB）
 * 计算修边后的画布矩形。上游 calculateTrimRect。
 *
 * - rgba：合成图像素，stride 为每行字节数（>= width * 4）；
 * - 没有任何可保留内容（整图透明 / 整图同基准色）返回 false（上游 nil）；
 * - 四边开关全关（trimsAny = false）返回 false（上游 guard）。
 *
 * 与上游逐行扫描算法一致：颜色基准模式下每行先从左找首个不匹配像素、
 * 再从右找，汇总四条边——不是对每个像素独立判断后取极值，两者在
 * 「行内中间有不匹配像素但两端匹配」时结果不同（上游保留整行），
 * 此处逐字照抄。
 */
bool trim_calculate_rect(const uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                         const TrimOptions& options, comp_rect* out_rect);

/*
 * 提交修边（上游 Image > Trim）：先 flatten 整篇合成计算边框，再按
 * crop_apply 的「平移图层 + 换画布」提交。一步撤销。
 *
 * - 无有效活动选区不影响修边（PS 的 Trim 不看选区）；
 * - 整篇没有可保留内容 → COMP_ERR_STATE（上游 noContentToTrim），文档不变；
 * - 无图层或空文档 → COMP_ERR_STATE；
 * - 修出的边框与当前画布相同 → out_changed = 0，不进历史；
 * - 换画布导致图层 origin 越界 → COMP_ERR_INVALID_ARGUMENT，文档不变
 *  （直接复用 crop_apply 的两遍校验）。
 */
comp_result document_trim(Document* doc, const TrimOptions& options, int32_t* out_changed,
                          std::string* error);

/* ------------------------------------------------------------ 像素反相 */

/*
 * 反相当前层像素（PS Image > Adjustments > Invert；上游 PixelInvert）。
 *
 * 预乘 RGBA 语义（上游 PixelInvert 注释）：每条颜色通道 c → alpha - c，
 * alpha 保持不变——透明像素反相后仍是透明的，不会变实心。
 *
 * - 显式活动选区存在时结果按 coverage 与原像素混合（同 filter_apply 的
 *   blend_through_selection，选区按文档坐标解释）；
 * - 空选区（active 但全 0）= 什么都不改，out_changed = 0；
 * - 16 位图层先落 8 位工作副本（deep_to_rgba8，同 filters 约定）；
 *   HDR（RGBA16F）图层拒绝（COMP_ERR_UNSUPPORTED）；
 * - 图层无像素（空层 / 纯组）拒绝（COMP_ERR_INVALID_ARGUMENT）。
 *
 * 与 COMP_ADJUSTMENT_INVERT 调整层的关系：调整层是非破坏可编辑的，
 * 本函数是破坏性一次性落墨，上游两者并存。
 */
comp_result invert_layer(Document* doc, comp_layer_id layer, int32_t* out_changed,
                         std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_CANVAS_OPS_H */

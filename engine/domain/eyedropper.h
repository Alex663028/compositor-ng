// AI生成
/*
 * eyedropper.h — 吸管取色（S5）。
 *
 * 对齐上游 README「Painting and retouching」最后一条：*Eyedropper and a full
 * color picker*。本层只做「在文档像素坐标处取一块颜色」，颜色模型（HSV、
 * 十六进制、色板）是宿主 UI 的事，放在 shells/windows/src/ui_core.h。
 *
 * 口径：
 *   - 坐标是**文档像素**（左上原点，允许小数；采样窗口按像素质心取整）；
 *   - 输出是**非预乘** sRGB 0..1（RGBA），宿主可直接当画笔颜色；
 *   - `sample_size` 是方形采样窗口边长（1..101，奇数），按预乘值平均后再
 *     反预乘——先反预乘再平均会在半透明边缘过亮；
 *   - 来源 0 = 目标图层本身，1 = 合并结果（与「Copy Merged」同一条合成通路）；
 *   - 16 位与 HDR 图层先走既有的 8 位工作副本（deep_to_rgba8 / hdr_to_rgba8），
 *     吸管拿到的就是屏幕上看到的颜色，与上游一致。
 *
 * 失败（画布/采样尺寸/来源非法、坐标非有限或落在画布外、图层不存在、图层无像素）
 * 返回 false 并写 error。
 */
#ifndef COMPOSITOR_DOMAIN_EYEDROPPER_H
#define COMPOSITOR_DOMAIN_EYEDROPPER_H

#include <string>

#include "compositor/comp_core.h"
#include "domain/document.h"

namespace compositor {

enum EyeDropperSource {
  kEyeDropperLayer = 0,
  kEyeDropperMerged = 1
};

constexpr int32_t kMaxEyeDropperSample = 101;

bool eyedropper_sample(const Document& document, comp_layer_id layer, double x, double y,
                       int32_t sample_size, int32_t source, float out_srgb[4],
                       std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_DOMAIN_EYEDROPPER_H */

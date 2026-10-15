// AI生成
/*
 * resample.h — 仿射重采样（旋转 / 缩放 / 翻转 + 蒙版烘焙）。
 *
 * 对齐上游 LayerTransform：
 *   - origin/size 决定图层在文档像素空间的目标矩形；
 *   - rotation 为顺时针角度（度）；flipX/flipY 为镜像；
 *   - sampling ∈ { "Nearest", "Smooth", "High quality" }（上游 rawValue，缺省 High quality）。
 *
 * 采样策略为「目标像素 -> 源像素」逆映射：对目标矩形内每个像素反解源坐标后取样，
 * 恒等情形（无旋转、尺寸相同）在整数对齐下逐字节精确，保证既有合成结果不漂移。
 *
 * 显式能力边界：旋转按目标矩形裁剪，超出矩形的内容不做包围盒扩张（保持「简洁好用」）。
 */
#ifndef COMPOSITOR_COMPOSITING_RESAMPLE_H
#define COMPOSITOR_COMPOSITING_RESAMPLE_H

#include <string>

#include "compositor/comp_core.h"
#include "core/comp_geometry.h"
#include "domain/layer.h"

namespace compositor {

/* 采样模式：与上游 .comp manifest 的 transform.sampling 一一对应。 */
enum class SamplingMode { kNearest = 0, kSmooth = 1, kHighQuality = 2 };

/* "Nearest" / "Smooth" / "High quality" —— 未知或缺省一律回退 High quality。 */
SamplingMode sampling_from_manifest_name(const std::string& name);
const char* sampling_manifest_name(SamplingMode mode);

/* ABI 枚举 <-> 内部模式。 */
SamplingMode sampling_from_abi(comp_sampling value);
comp_sampling sampling_to_abi(SamplingMode mode);

/*
 * 把图层像素重采样为「目标矩形空间」的图层单元缓冲：
 *   - out 尺寸 = bounds.w × bounds.h，COMP_PIXEL_RGBA8 预乘；
 *   - 已烘焙翻转、旋转、缩放与蒙版（蒙版按最近采样乘入 alpha 与颜色）；
 *   - 源外区域保持全透明。
 * out_bounds 返回目标矩形（layer.effective_bounds()）。失败写 error 并返回 false。
 */
bool build_layer_unit(const Layer& layer, SamplingMode mode, ImageBuffer* out,
                      comp_rect* out_bounds, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_RESAMPLE_H */

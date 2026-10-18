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
#include <vector>

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
 *
 * clip 可为 nullptr（光栅化完整 bounds）。传入非空值时只光栅化 bounds ∩ clip
 * 这一块，out_bounds 一并改报该窗口；合成路径传 clamp_to_canvas 后的可见矩形，
 * 使中间缓冲以可见区域为界（上游 TiledLayerRenderer 同思路：从不为一层实体化
 * 整幅光栅）。裁剪不改变任何像素值——逆映射分母始终用完整 bounds，见实现注释。
 *
 * out_bounds 返回目标矩形。失败写 error 并返回 false。
 */
bool build_layer_unit(const Layer& layer, SamplingMode mode, ImageBuffer* out,
                      comp_rect* out_bounds, std::string* error,
                      const comp_rect* clip = nullptr);

/*
 * C5：与 build_layer_unit 同一套几何/采样口径，输出 **half（COMP_PIXEL_RGBA16F）**
 * 单元缓冲，供 half 合成表面叠加。
 *
 *   - 源可以是 RGBA16F（直接按 half 解码）、RGBA16（v/65535）或 RGBA8（v/255）；
 *   - 采样在 double 里做，**颜色不夹到 [0,1]**：HDR 的白点以上亮度必须留在单元里。
 *     负值和 NaN 落 0（预乘缓冲不接受负颜色），alpha 夹进 [0,1]；
 *   - 蒙版按最近采样乘进四个分量；
 *   - 恒等情形（无旋转、尺寸相同）逐半精度精确：nearest 直接拷贝，bilinear/bicubic
 *     在整数坐标处的权重退化为 1/0。
 *
 * 几何部分与 build_layer_unit 逐行同构（同一组 doc->source 逆映射），因为两条路
 * 若各算一套坐标，同一个图层在 8 位与 half 表面上会落在不同的像素上。测试
 * test_c5_half_surface 用「同一图层分别走两条路」钉住这一点。
 */
bool build_layer_unit_hdr(const Layer& layer, SamplingMode mode, ImageBuffer* out,
                          comp_rect* out_bounds, std::string* error,
                          const comp_rect* clip = nullptr);

/*
 * 把普通 RGBA8 预乘缓冲（单位几何：无旋转/翻转/掩版，像素即内容）重采样到
 * 目标尺寸。用于 Image Size（对齐上游 ImageResizer）中已光栅化层单元的整幅
 * 缩放：逆映射 + 与 build_layer_unit 同一组采样内核；目标尺寸与源相同时在
 * 整数对齐下逐字节精确（缩放路径的安全属性）。源外坐标按边缘夹取——整幅
 * 缩放不该像图层贴图那样引入透明边。src 为空或目标尺寸为 0 返回 false。
 */
bool resample_image(const ImageBuffer& src, int32_t dst_w, int32_t dst_h,
                    SamplingMode mode, ImageBuffer* out);

/*
 * 单通道（掩版）重采样：同一套 nearest/bilinear/bicubic 内核的灰度版。
 * 1x1 的 uniform 掩版分辨率无关，由调用方保持原样，不进本函数。
 * src 为空或目标尺寸为 0 返回 false。
 */
bool resample_gray(const uint8_t* src, int32_t src_w, int32_t src_h,
                   int32_t dst_w, int32_t dst_h, SamplingMode mode,
                   std::vector<uint8_t>* out);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_RESAMPLE_H */

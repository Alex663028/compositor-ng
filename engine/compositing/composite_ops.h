// AI生成
/*
 * composite_ops.h — 合成基础算子（引擎内部共享）。
 *
 * 从 compositor.cpp 抽出，供「顺序合成路径」与「FilterGraph 有向图」复用同一套
 * 像素语义，避免两条路径各自实现混合公式而出现分歧：
 *   - 预乘 <-> 直通换算与单像素混合（W3C Compositing and Blending Level 1）；
 *   - 蒙版系数、剪贴基底系数的求值；
 *   - 图层缓冲 / 任意画布缓冲叠加到目标。
 *
 * 本头文件属于引擎内部实现，不进入 C ABI（comp_*.h）。
 */
#ifndef COMPOSITOR_COMPOSITING_COMPOSITE_OPS_H
#define COMPOSITOR_COMPOSITING_COMPOSITE_OPS_H

#include "compositor/comp_core.h"
#include "domain/document.h"
#include "domain/layer.h"

namespace compositor {

/* 0..1 的直通（非预乘）颜色 + alpha。 */
struct StraightPixel {
  double rgb[3];
  double alpha;
};

/* 预乘 RGBA8 像素 -> 直通分量（a == 0 时颜色视为 0）。 */
StraightPixel to_straight(const uint8_t* p);

uint8_t to_u8(double v);

/* 直通颜色 + alpha -> 预乘 RGBA8。 */
void write_premultiplied(uint8_t* p, const double rgb[3], double alpha);

/*
 * 单像素混合：把 src_px 按 src_alpha 混合进 dst_px。
 * src_alpha 为 0..1，已包含图层不透明度、组穿透系数、蒙版与剪贴的综合结果。
 */
void blend_pixels(uint8_t* dst_px, const uint8_t* src_px, comp_blend_mode mode, double src_alpha);

/*
 * 单像素混合的**外部可链接公共入口**（薄包装，直接转调 blend_pixels）。
 *
 * 为什么单列一个入口：测试与壳侧调用方若自行书写前置声明，容易因命名空间/签名不一致
 * 在链接期找不到符号（曾出现 `lld-link: error: undefined symbol: blend_pixels(...)`）。
 * 统一走本入口后，「预乘修正」这类逐字节语义断言可以稳定地钉在实现上，
 * 无需依赖内部符号的链接可见性。
 */
void compositor_blend_pixel_scalar(uint8_t* dst_px, const uint8_t* src_px, comp_blend_mode mode,
                                   double src_alpha);

/* 取剪贴基底图层在文档坐标处的 alpha（找不到返回 1.0，表现为不裁剪）。 */
double clipping_factor(const Document& doc, const Layer& layer, int32_t x, int32_t y);

/*
 * 把图层的像素（含 bounds / 翻转 / 蒙版 / 剪贴）叠加到 dst。
 * opacity_scale 为上层穿透组累积的不透明度系数（顶层调用传 1.0）。
 * 图层组自身不产生像素，直接返回 true——组的聚合由 FilterGraph 负责。
 */
bool blend_layer_into(ImageBuffer* dst, const Document& doc, const Layer& layer, double opacity_scale);

/*
 * 把一个与画布同尺寸的缓冲整体叠加到 dst：用于「隔离组」把子树结果作为
 * 一个单元参与混合。src 与 dst 尺寸必须一致，mode/opacity 取组的混合模式与不透明度。
 */
bool blend_buffer_into(ImageBuffer* dst, const ImageBuffer& src, comp_blend_mode mode, double opacity);

}  // namespace compositor

#endif /* COMPOSITOR_COMPOSITING_COMPOSITE_OPS_H */

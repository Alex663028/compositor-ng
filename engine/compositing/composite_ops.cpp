// AI生成
#include "compositing/composite_ops.h"

#include "compositing/blend.h"
#include "compositing/resample.h"
#include "core/comp_geometry.h"

namespace compositor {

StraightPixel to_straight(const uint8_t* p) {
  StraightPixel out{};
  const double a = static_cast<double>(p[3]) / 255.0;
  out.alpha = a;
  if (a > 0.0) {
    for (int i = 0; i < 3; ++i) {
      out.rgb[i] = (static_cast<double>(p[i]) / 255.0) / a;
    }
  } else {
    out.rgb[0] = out.rgb[1] = out.rgb[2] = 0.0;
  }
  return out;
}

uint8_t to_u8(double v) {
  if (v <= 0.0) return 0;
  if (v >= 1.0) return 255;
  return static_cast<uint8_t>(v * 255.0 + 0.5);
}

void write_premultiplied(uint8_t* p, const double rgb[3], double alpha) {
  const double a = alpha <= 0.0 ? 0.0 : (alpha >= 1.0 ? 1.0 : alpha);
  for (int i = 0; i < 3; ++i) {
    const double straight = rgb[i] < 0.0 ? 0.0 : (rgb[i] > 1.0 ? 1.0 : rgb[i]);
    p[i] = to_u8(straight * a);
  }
  p[3] = to_u8(a);
}

/* 写入「已经是预乘」的合成结果。
 *
 * 与 write_premultiplied 的区别：本函数不再乘 alpha。
 * blend_pixels 按 W3C 公式算出的 out_rgb 本身已含 alpha 权重，
 * 若再走 write_premultiplied 就会二次乘 alpha（alpha<1 时结果被压暗）。 */
static void store_premultiplied(uint8_t* p, const double premul_rgb[3], double alpha) {
  const double a = alpha <= 0.0 ? 0.0 : (alpha >= 1.0 ? 1.0 : alpha);
  for (int i = 0; i < 3; ++i) {
    const double v = premul_rgb[i] < 0.0 ? 0.0 : (premul_rgb[i] > 1.0 ? 1.0 : premul_rgb[i]);
    p[i] = to_u8(v);
  }
  p[3] = to_u8(a);
}

void blend_pixels(uint8_t* dst_px, const uint8_t* src_px, comp_blend_mode mode, double src_alpha) {
  const StraightPixel src = to_straight(src_px);
  const StraightPixel base = to_straight(dst_px);

  double blended[3];
  blend_rgb(mode, base.rgb, src.rgb, blended);

  /* W3C 合成公式；结果即为预乘值。 */
  double out_rgb[3];
  const double out_alpha = src_alpha + base.alpha * (1.0 - src_alpha);
  for (int i = 0; i < 3; ++i) {
    out_rgb[i] = src_alpha * (1.0 - base.alpha) * src.rgb[i] +
                 src_alpha * base.alpha * blended[i] +
                 (1.0 - src_alpha) * base.alpha * base.rgb[i];
  }
  store_premultiplied(dst_px, out_rgb, out_alpha);
}

/* 外部可链接的公共入口：薄包装，语义与 blend_pixels 完全一致（不做任何额外换算）。 */
void compositor_blend_pixel_scalar(uint8_t* dst_px, const uint8_t* src_px, comp_blend_mode mode,
                                   double src_alpha) {
  blend_pixels(dst_px, src_px, mode, src_alpha);
}

double clipping_factor(const Document& doc, const Layer& layer, int32_t x, int32_t y) {
  if (layer.clipping_source == 0) return 1.0;
  const Layer* base = doc.find(layer.clipping_source);
  if (base == nullptr || base->pixels.empty()) return 1.0;
  const comp_rect bounds = base->effective_bounds();
  const int32_t lx = x - bounds.x;
  const int32_t ly = y - bounds.y;
  if (lx < 0 || ly < 0 || lx >= base->pixels.width || ly >= base->pixels.height) return 0.0;
  const uint8_t* px = base->pixels.row(ly) + static_cast<size_t>(lx) * 4;
  const double base_alpha = (static_cast<double>(px[3]) / 255.0) *
                            static_cast<double>(base->opacity);
  return base_alpha;
}

bool blend_layer_into(ImageBuffer* dst, const Document& doc, const Layer& layer, double opacity_scale) {
  if (dst == nullptr) return false;
  if (layer.visible == false) return true;
  /* 图层组不直接持有像素：其聚合由 FilterGraph 处理。 */
  if (layer.is_group) return true;
  if (layer.pixels.empty()) return true;
  if (!is_blend_supported(layer.blend)) return true;

  const comp_rect bounds = layer.effective_bounds();
  if (is_empty(bounds)) return true;
  const comp_rect dst_bounds = clamp_to_canvas(bounds, comp_size{dst->width, dst->height});
  if (is_empty(dst_bounds)) return true;

  /*
   * 统一走仿射重采样：把图层像素（含翻转/旋转/缩放/蒙版）烘焙为目标矩形空间的
   * 一个单元缓冲，再叠加到目标。恒等情形（无旋转、尺寸相同）逐字节精确，
   * 保证 M1 既有合成结果不漂移；采样模式由 layer.sampling 决定。
   */
  ImageBuffer unit;
  std::string error;
  if (!build_layer_unit(layer, sampling_from_abi(layer.sampling), &unit, nullptr, &error)) {
    return false;
  }
  if (unit.empty()) return true;

  const double layer_opacity = static_cast<double>(layer.opacity) * opacity_scale;

  for (int32_t y = dst_bounds.y; y < dst_bounds.y + dst_bounds.h; ++y) {
    uint8_t* dst_row = dst->writable_row(y);
    const uint8_t* unit_row = unit.row(y - bounds.y);
    for (int32_t x = dst_bounds.x; x < dst_bounds.x + dst_bounds.w; ++x) {
      const uint8_t* src_px = unit_row + static_cast<size_t>(x - bounds.x) * 4;
      double alpha = (static_cast<double>(src_px[3]) / 255.0) * layer_opacity;
      if (alpha <= 0.0) continue;
      alpha *= clipping_factor(doc, layer, x, y);
      if (alpha <= 0.0) continue;

      blend_pixels(dst_row + static_cast<size_t>(x) * 4, src_px, layer.blend, alpha);
    }
  }
  return true;
}

bool blend_buffer_into(ImageBuffer* dst, const ImageBuffer& src, comp_blend_mode mode, double opacity) {
  if (dst == nullptr) return false;
  if (dst->width != src.width || dst->height != src.height) return false;
  if (!is_blend_supported(mode)) return true;

  const double clamped_opacity = opacity <= 0.0 ? 0.0 : (opacity >= 1.0 ? 1.0 : opacity);
  if (clamped_opacity <= 0.0) return true;

  for (int32_t y = 0; y < dst->height; ++y) {
    uint8_t* dst_row = dst->writable_row(y);
    const uint8_t* src_row = src.row(y);
    for (int32_t x = 0; x < dst->width; ++x) {
      const uint8_t* src_px = src_row + static_cast<size_t>(x) * 4;
      const double alpha = (static_cast<double>(src_px[3]) / 255.0) * clamped_opacity;
      if (alpha <= 0.0) continue;
      blend_pixels(dst_row + static_cast<size_t>(x) * 4, src_px, mode, alpha);
    }
  }
  return true;
}

}  // namespace compositor

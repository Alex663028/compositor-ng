// AI生成
#include "compositing/composite_ops.h"

#include <cstring>
#include <vector>

#include "compositing/blend.h"
#include "compositing/effect_eval.h"
#include "compositing/resample.h"
#include "core/comp_geometry.h"
#include "imaging/BrushPixels.h"
#include "imaging/DeepPixels.h"

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
  /*
   * alpha 按存储格式的实际字节宽取：写死 4 会在 16 位基底上读到像素中间的字节，
   * 裁剪因子变成随机数（同一处错误在 trim_to_content 已经修过，这里跟上）。
   * 16 位用与内核 dither_quantize16 相同的取整口径（round(a16 * 255 / 65535)），
   * 因此「直接读 16 位基底」与「读它的 8 位副本」得到同一个因子——
   * 合成结果与截断到 8 位再合成逐像素一致。
   */
  const int32_t bpp = bytes_per_pixel(base->pixels.format);
  if (bpp == 4) {
    const uint8_t* px = base->pixels.row(ly) + static_cast<size_t>(lx) * 4u;
    return (static_cast<double>(px[3]) / 255.0) * static_cast<double>(base->opacity);
  }
  if (bpp == 8) {
    const uint8_t* px = base->pixels.row(ly) + static_cast<size_t>(lx) * 8u;
    uint16_t a16 = 0;
    std::memcpy(&a16, px + 6, sizeof(a16)); /* native-endian：alpha 在每像素最后两个字节 */
    const int32_t a8 = (static_cast<int32_t>(a16) + 128) / 257;
    return (static_cast<double>(a8) / 255.0) * static_cast<double>(base->opacity);
  }
  /* 未知格式：按「无裁剪」处理，与 base 缺失同口径。 */
  return 1.0;
}

/*
 * 剪贴栈的 alpha 拆合（对齐上游 LiveMaskRenderer.drawComposite:110-150）。
 *
 * 上游口径不是「把子层按基底的 alpha 逐像素缩放」，而是：
 *   1. 基底 alpha 存进 coverage，颜色**拉成不透明**（extract_stack_alpha）；
 *   2. 子层在**不透明**的基底上按各自混合模式叠；
 *   3. 把 coverage 放回去（restore_stack_alpha）。
 * 差别在非 Normal 混合：逐像素缩放等于两次半透明的 source-over，叠出来的 alpha
 * 会归 0（实测软 alpha 基底 + MULTIPLY 子层得 a=0），而且混合语义完全不对；
 * 「不透明基底 + 混合 + 还原 alpha」才是 Photoshop 的剪贴行为——子层在基底**里面**
 * 着色。上游注释也点明了原因：“Clipping stacks share the base's alpha instead of
 * painting that alpha over itself”。
 *
 * coverage 只存 alpha，**不**乘基底 opacity——opacity 在 blend_layer_into 的
 * opacity_scale 里另算，与上游一致。
 *
 * 这两个助手只管「表面」，不负责合成基底/成员；谁把它们串起来（FilterGraph 的
 * kClippingStack 节点）由调用方决定，因为 8/16 位表面与栈的包围盒都由那边决定。
 */
bool extract_stack_alpha(ImageBuffer* surface, std::vector<uint16_t>* coverage16, std::string* error) {
  if (surface == nullptr || surface->empty()) {
    if (error != nullptr) *error = "clipping stack surface is empty";
    return false;
  }
  const size_t w = static_cast<size_t>(surface->width);
  const size_t h = static_cast<size_t>(surface->height);
  uint8_t* pixels = surface->writable_pixels().data();
  const size_t stride = surface->stride();
  if (surface->format == COMP_PIXEL_RGBA16) {
    if (coverage16 == nullptr) {
      if (error != nullptr) *error = "clipping stack needs a 16-bit coverage buffer";
      return false;
    }
    coverage16->assign(w * h, 0);
    deep_extract_alpha(reinterpret_cast<uint16_t*>(pixels), stride, coverage16->data(), w, h);
    return true;
  }
  if (surface->format != COMP_PIXEL_RGBA8) {
    if (error != nullptr) *error = "clipping stack surface has an unsupported pixel format";
    return false;
  }
  std::vector<uint8_t> scratch;
  scratch.assign(w * h, 0);
  layer_extract_alpha(pixels, stride, scratch.data(), w, w, h);
  layer_unpremultiply_opaque(pixels, stride, w, h);
  /* 8 位 coverage 拓宽到 16 位缓冲：两个宽度的 alpha 量纲一致（0..1），这样
   * 调用方不必为两种底色各写一份 coverage 容器。 */
  if (coverage16 != nullptr) {
    coverage16->resize(w * h);
    for (size_t i = 0; i < coverage16->size(); ++i) {
      (*coverage16)[i] = static_cast<uint16_t>(static_cast<uint32_t>(scratch[i]) * 257u);
    }
  }
  return true;
}

/* 还原上一步取走的 alpha（`layer_restore_alpha` / `deep_restore_alpha`）。 */
bool restore_stack_alpha(ImageBuffer* surface, const std::vector<uint16_t>& coverage16,
                         std::string* error) {
  if (surface == nullptr || surface->empty()) {
    if (error != nullptr) *error = "clipping stack surface is empty";
    return false;
  }
  const size_t w = static_cast<size_t>(surface->width);
  const size_t h = static_cast<size_t>(surface->height);
  uint8_t* pixels = surface->writable_pixels().data();
  const size_t stride = surface->stride();
  if (coverage16.size() != w * h) {
    if (error != nullptr) *error = "clipping stack coverage size does not match the surface";
    return false;
  }
  if (surface->format == COMP_PIXEL_RGBA16) {
    deep_restore_alpha(reinterpret_cast<uint16_t*>(pixels), stride, coverage16.data(), w, h);
    return true;
  }
  if (surface->format != COMP_PIXEL_RGBA8) {
    if (error != nullptr) *error = "clipping stack surface has an unsupported pixel format";
    return false;
  }
  std::vector<uint8_t> scratch(w * h);
  for (size_t i = 0; i < scratch.size(); ++i) {
    scratch[i] = static_cast<uint8_t>(coverage16[i] / 257u);
  }
  layer_restore_alpha(pixels, stride, scratch.data(), w, w, h);
  return true;
}

bool blend_layer_into(ImageBuffer* dst, const Document& doc, const Layer& layer,
                      double opacity_scale, std::string* error) {
  if (dst == nullptr) {
    if (error != nullptr) *error = "destination buffer is null";
    return false;
  }
  if (layer.visible == false) return true;
  /* 图层组不直接持有像素：其聚合由 FilterGraph 处理。 */
  if (layer.is_group) return true;
  if (layer.pixels.empty()) return true;
  if (!is_blend_supported(layer.blend)) return true;

  const comp_rect bounds = layer.effective_bounds();
  if (is_empty(bounds)) return true;

  /*
   * 效果需要画在图层**周围**，因此单元缓冲必须比图层 bounds 大出效果半径，否则
   * 投影 / 外发光 / outside 描边会被 bounds 裁掉（上游 LayerEffectsRenderer.margin
   * 正是为这件事把 transform 外扩）。
   *
   * 这里外扩 bounds 再取交：单元缓冲变大 → shape（呈现形状）在同一画布上仍是正确
   * 的文档坐标像素，效果半径内的画布也随之进入单元 —— 于是 apply_effects 就地求值
   * 无需知道图层几何，也不会在“图层不透明区域之外”误画。
   *
   * 代价：带大模糊的投影会把单元缓冲放大到 bounds 的数倍。这是内存/时间上的取舍，
   * 与上游“效果 inset 扩大 transform”同源；先取交画布，避免一个把图层拖出画布很远
   * 的大效果分配出整幅巨大的中间缓冲。
   */
  const int32_t margin = effects_margin(layer.effects);
  const comp_rect padded = margin > 0
                               ? comp_rect{bounds.x - margin, bounds.y - margin,
                                           bounds.w + margin * 2, bounds.h + margin * 2}
                               : bounds;
  const comp_rect dst_bounds = clamp_to_canvas(padded, comp_size{dst->width, dst->height});
  if (is_empty(dst_bounds)) return true;

  /*
   * 统一走仿射重采样：把图层像素（含翻转/旋转/缩放/蒙版）烘焙为目标矩形空间的
   * 一个单元缓冲，再叠加到目标。恒等情形（无旋转、尺寸相同）逐字节精确，
   * 保证 M1 既有合成结果不漂移；采样模式由 layer.sampling 决定。
   */
  ImageBuffer unit;
  std::string unit_error;
  /* 只光栅化可见窗口（dst_bounds）：中间缓冲以画布内可见区域为界，
   * 不再为一个拉到画布外的巨大图层分配整幅单元（上游 TiledLayerRenderer 同思路）。 */
  comp_rect unit_bounds;
  if (!build_layer_unit(layer, sampling_from_abi(layer.sampling), &unit, &unit_bounds,
                        &unit_error, &dst_bounds)) {
    /* 内因透传：调用点需要知道是「单元太大」还是别的，直接丢给 error。 */
    if (error != nullptr) *error = std::move(unit_error);
    return false;
  }
  if (unit.empty()) return true;

  /*
   * 效果求值（对齐上游 LayerEffects.swift:388 `render(_:mask:effects:)`）：
   * 就地作用在单元缓冲上，顺序是「先做效果，再当普通图层混合」——与上游一致，
   * 效果因此不吃图层的混合模式与不透明度（在 blend_pixels 那一步才生效）。
   *
   * effects 为空时这里是零成本的早退（不分配、不改像素），既有渲染逐字节不变。
   */
  if (!layer.effects.empty()) {
    EffectEvalResult effect_result;
    std::string effect_error;
    if (!apply_effects(layer.effects, unit.writable_pixels().data(), unit.width, unit.height,
                        unit.stride(), &effect_result, &effect_error)) {
      if (error != nullptr) {
        *error = effect_error.empty() ? "apply effects failed" : effect_error;
      }
      return false;
    }
  }

  const double layer_opacity = static_cast<double>(layer.opacity) * opacity_scale;

  for (int32_t y = dst_bounds.y; y < dst_bounds.y + dst_bounds.h; ++y) {
    uint8_t* dst_row = dst->writable_row(y);
    /* 单元缓冲以 unit_bounds（= bounds ∩ dst_bounds）为原点，与文档坐标差一个偏移。 */
    const uint8_t* unit_row = unit.row(y - unit_bounds.y);
    for (int32_t x = dst_bounds.x; x < dst_bounds.x + dst_bounds.w; ++x) {
      const uint8_t* src_px = unit_row + static_cast<size_t>(x - unit_bounds.x) * 4;
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

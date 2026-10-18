// AI生成
#include "domain/layer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imaging/comp_imaging.h"

namespace compositor {

void ImageBuffer::resize(int32_t w, int32_t h, comp_pixel_format fmt) {
  width = std::max(0, w);
  height = std::max(0, h);
  format = fmt;
  writable_pixels().assign(byte_size(), 0);
}

void ImageBuffer::fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (empty()) return;
  const int32_t bpp = bytes_per_pixel(format);
  if (bpp != 4) {
    /* M1 只支持 RGBA8 的填充；其余格式留待 M2 的高位深路径。 */
    std::vector<uint8_t>& dst = writable_pixels();
    std::memset(dst.data(), 0, dst.size());
    return;
  }
  /*
   * RGBA8 缓冲是**预乘**的（blend_pixels / layer_restore_alpha 都这么读），
   * 而 solid/fill 的入参是「直通色」。因此半透明实色必须在这里预乘：
   * 直接写 r,g,b 会让「纯红 50%」的像素变成 rgb=(255,0,0) alpha=128——比 alpha
   * 还亮的预乘值。它自身看着还行，但一旦过 extract/restore（剪贴栈）或
   * non-normal 混合，就会被当成不透明色，颜色整体偏亮（本轮实测：软 alpha 基底
   * + MULTIPLY 成员得到 a=0 之外的错误 RGB）。
   *
   * 舍入用与 blend 层一致的 (c*a + 127) / 255，保证 solid 出来的像素与
   * 「同色不透明像素按 alpha 缩放」逐字节相同。
   */
  const unsigned rr = (static_cast<unsigned>(r) * a + 127u) / 255u;
  const unsigned gg = (static_cast<unsigned>(g) * a + 127u) / 255u;
  const unsigned bb = (static_cast<unsigned>(b) * a + 127u) / 255u;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* p = writable_row(y);
    for (int32_t x = 0; x < width; ++x) {
      p[0] = static_cast<uint8_t>(rr);
      p[1] = static_cast<uint8_t>(gg);
      p[2] = static_cast<uint8_t>(bb);
      p[3] = a;
      p += 4;
    }
  }
}

ImageBuffer ImageBuffer::solid(int32_t w, int32_t h, comp_pixel_format fmt,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  ImageBuffer img;
  img.resize(w, h, fmt);
  img.fill(r, g, b, a);
  return img;
}

bool deep_to_rgba8(const ImageBuffer& src, ImageBuffer* dst) {
  if (dst == nullptr) return false;
  if (src.format != COMP_PIXEL_RGBA16 || src.empty()) return false;
  const size_t w = static_cast<size_t>(src.width);
  const size_t h = static_cast<size_t>(src.height);
  std::vector<uint8_t> out(w * h * 4u, 0);
  /*
   * 内核按 width * 4 个 uint16 步进，等于 ImageBuffer 在 RGBA16 下的 stride，
   * 所以 row(0) 就是它要的基址。结果先落在本地缓冲：src 与 dst 允许是同一个
   * 对象（deep quantize 就地转换），先算完再重置 dst 才不会边读边写。
   */
  dither_quantize16(reinterpret_cast<const uint16_t*>(src.row(0)), out.data(), w, h, w * 4u);
  dst->resize(src.width, src.height, COMP_PIXEL_RGBA8);
  const size_t row_bytes = w * 4u;
  for (size_t y = 0; y < h; ++y) {
    std::memcpy(dst->writable_row(static_cast<int32_t>(y)), out.data() + y * row_bytes, row_bytes);
  }
  return true;
}

bool hdr_to_rgba8(const ImageBuffer& src, ImageBuffer* dst) {
  if (dst == nullptr) return false;
  if (src.format != COMP_PIXEL_RGBA16F || src.empty()) return false;
  const size_t w = static_cast<size_t>(src.width);
  const size_t h = static_cast<size_t>(src.height);
  /*
   * 两趟，与上游 `dithered(image(hdr))` 同序：
   *   1. `deep_half_to_u16`：半精度的**值**按 0..65535 取整写进 16 位布局
   *      （white 以上的光在此截断，DeepLayers.swift:31 的 canonical/image）。
   *   2. `dither_quantize16`：在这份 16 位布局上加约一阶噪声后取整成 8 位
   *      （DeepLayers.swift:37 的 dithered）。
   * 两趟不能合并：把 half 的位模式直接当 16 位整数取整，0x3C00（1.0）会得 15360
   * 而不是 255——这正是「高光当噪声、画面全错」的那类错误。
   *
   * 两个中间缓冲都先落在本地：src 与 dst 允许是同一个对象，先算完再重置 dst
   * 才不会边读边写。
   */
  std::vector<uint16_t> wide(w * h * 4u, 0);
  deep_half_to_u16(reinterpret_cast<const uint16_t*>(src.row(0)), src.stride(), wide.data(), w * 8u, w, h);
  std::vector<uint8_t> out(w * h * 4u, 0);
  dither_quantize16(wide.data(), out.data(), w, h, w * 4u);
  dst->resize(src.width, src.height, COMP_PIXEL_RGBA8);
  const size_t row_bytes = w * 4u;
  for (size_t y = 0; y < h; ++y) {
    std::memcpy(dst->writable_row(static_cast<int32_t>(y)), out.data() + y * row_bytes, row_bytes);
  }
  return true;
}

bool rgba8_to_hdr(const ImageBuffer& src, ImageBuffer* dst) {
  if (dst == nullptr) return false;
  if (src.format != COMP_PIXEL_RGBA8 || src.empty()) return false;
  if (dst->width != src.width || dst->height != src.height || dst->format != COMP_PIXEL_RGBA16F) {
    return false;
  }
  /*
   * 8 位工作副本写回 HDR 图层：**不回填高光**。上游的口径是 8 位工具用完后图层
   * 整体降为 8 位（DeepLayers.swift:297 "they're dithered down, halving the
   * layer's memory and file size"），因此这一步只承诺「把 8 位值按分量写成半精度」，
   * 之前被截断的高光不会回来。要保留高光必须走 hdr_normalize/restore 那对内核，
   * 那是 C2 的事。
   */
  const size_t w = static_cast<size_t>(src.width);
  const size_t h = static_cast<size_t>(src.height);
  std::vector<uint16_t> wide(w * h * 4u, 0);
  for (size_t y = 0; y < h; ++y) {
    const uint8_t* in = src.row(static_cast<int32_t>(y));
    uint16_t* o = wide.data() + y * w * 4u;
    for (size_t x = 0; x < w * 4u; ++x) o[x] = static_cast<uint16_t>(in[x] * 257u);
  }
  std::vector<uint16_t> half(w * h * 4u, 0);
  deep_u16_to_half(wide.data(), w * 8u, half.data(), w * 8u, w, h);
  for (size_t y = 0; y < h; ++y) {
    std::memcpy(dst->writable_row(static_cast<int32_t>(y)), half.data() + y * w * 4u * sizeof(uint16_t),
                w * 4u * sizeof(uint16_t));
  }
  return true;
}

/*
 * `DeepLayer.wideAsset`（DeepLayers.swift:174）那道闸的值：白点以上千分之一。
 * 上游原话：「A thousandth over: a file brushing white through rounding isn't HDR.」
 */
constexpr float kWideAssetPeak = 1.001f;

bool wide_asset_from_linear(const float* rgba, int32_t width, int32_t height, ImageBuffer* out) {
  if (out == nullptr || rgba == nullptr) return false;
  if (width <= 0 || height <= 0) return false;
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);

  /* 第一趟：按 HDR 图层的布局收一遍（sRGB 曲线过白点延伸、乘 alpha、写 half）。
   * 两个中间缓冲都先落在本地：调用方的浮点源缓冲可能正来自 out 之外的文件缓冲，
   * 而且 out 会被 resize 重置，先算完再重置才不会边读边写。 */
  std::vector<uint16_t> half(w * h * 4u, 0u);
  deep_hdr_take_linear(rgba, w * 16u, half.data(), w * 8u, width, height);

  /* 判决在这份 half 上做，与上游一致：`deep_half_peak` 取的是反预乘后的最亮分量
   * （white = 1），对同一份像素做判决与做存储，不会出现「判成 HDR 但峰值为 1」。 */
  const float peak = deep_half_peak(half.data(), w * 8u, width, height);
  if (peak > kWideAssetPeak) {
    out->resize(width, height, COMP_PIXEL_RGBA16F);
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(out->writable_row(y), half.data() + static_cast<size_t>(y) * w * 4u, w * 4u * sizeof(uint16_t));
    }
    return true;
  }

  /* 不到 1.001：不是 HDR 素材，按 16 位整数层收——上游 else 支的
   * `asset(image(pixels))`（同一份 half 按 0..65535 取值，white 处即 65535），
   * 保留它比 half 更细的整数存储，而它本来没有高光可保（见层头注释）。 */
  std::vector<uint16_t> wide(w * h * 4u, 0u);
  deep_half_to_u16(half.data(), w * 8u, wide.data(), w * 8u, width, height);
  out->resize(width, height, COMP_PIXEL_RGBA16);
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(out->writable_row(y), wide.data() + static_cast<size_t>(y) * w * 4u, w * 4u * sizeof(uint16_t));
  }
  return true;
}

void MaskBuffer::resize(int32_t w, int32_t h, uint8_t fill_value) {
  width = std::max(0, w);
  height = std::max(0, h);
  writable_pixels().assign(byte_size(), fill_value);
}

void MaskBuffer::fill(uint8_t value) {
  std::vector<uint8_t>& dst = writable_pixels();
  /* 空掩码时 data() 可以是 nullptr：memset(nullptr, v, 0) 虽是"无害的"技术 UB，
   * 也没有像素可填，直接早退。 */
  if (dst.empty()) return;
  std::memset(dst.data(), value, dst.size());
}

comp_rect Layer::effective_bounds() const {
  if (clipped_empty) return comp_rect{transform.x, transform.y, 0, 0};
  if (transform.w > 0 && transform.h > 0) return transform;
  /* transform 的 w/h 未显式给出时回落到像素尺寸；
   * 但原点 (x, y) 始终以 transform 为准——它表达图层在画布上的落位，
   * 不能随尺寸回落一起被清零，否则带偏移的图层会被错误地画到左上角。 */
  return comp_rect{transform.x, transform.y, pixels.width, pixels.height};
}

bool Layer::document_to_pixel(int32_t source_width, int32_t source_height, double doc_x,
                              double doc_y, int32_t* out_x, int32_t* out_y) const {
  if (out_x == nullptr || out_y == nullptr) return false;
  if (source_width <= 0 || source_height <= 0) return false;
  const comp_rect bounds = effective_bounds();
  if (bounds.w <= 0 || bounds.h <= 0) return false;

  const double ud = (doc_x + 0.5 - static_cast<double>(bounds.x)) /
                        static_cast<double>(bounds.w) * 2.0 - 1.0;
  const double vd = (doc_y + 0.5 - static_cast<double>(bounds.y)) /
                        static_cast<double>(bounds.h) * 2.0 - 1.0;

  double us = ud;
  double vs = vd;
  if (std::fabs(static_cast<double>(rotation)) > 1e-9) {
    constexpr double kPi = 3.14159265358979323846;
    const double rad = static_cast<double>(rotation) * kPi / 180.0;
    const double cos_t = std::cos(rad);
    const double sin_t = std::sin(rad);
    us = ud * cos_t + vd * sin_t;
    vs = -ud * sin_t + vd * cos_t;
  }
  if (flip_x) us = -us;
  if (flip_y) vs = -vs;

  const double sx = (us + 1.0) * 0.5 * static_cast<double>(source_width) - 0.5;
  const double sy = (vs + 1.0) * 0.5 * static_cast<double>(source_height) - 0.5;
  if (sx < -0.5 || sy < -0.5 || sx > static_cast<double>(source_width) - 0.5 ||
      sy > static_cast<double>(source_height) - 0.5) {
    return false;
  }
  int32_t ix = static_cast<int32_t>(std::lround(sx));
  int32_t iy = static_cast<int32_t>(std::lround(sy));
  if (ix < 0) ix = 0;
  if (iy < 0) iy = 0;
  if (ix >= source_width) ix = source_width - 1;
  if (iy >= source_height) iy = source_height - 1;
  *out_x = ix;
  *out_y = iy;
  return true;
}

}  // namespace compositor

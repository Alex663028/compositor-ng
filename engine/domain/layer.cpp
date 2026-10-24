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

/*
 * 锁定透明像素的还原口径（P1-2 第一批，见 Layer::lock_flags）。
 *
 * 绘画类内核是「整幅 8 位就地写」，没有逐笔的 alpha 栅栏；锁的语义在事后统一
 * 落一遍，比让 20 个算子各自记得保住 alpha 更可靠：
 *   1. 原 alpha 为 0 的像素：整份还原（落在透明处的笔迹被丢弃）；
 *   2. 落笔把像素抹成全透明的：整份还原（锁透明时橡皮/擦除不生效）；
 *   3. 其余像素：alpha 还原成原值，新 RGB 按 old_alpha / new_alpha 重新预乘
 *      （缓冲区是**预乘** RGBA8，直接留 new RGB 会得到比 alpha 还亮的「过曝」
 *      预乘值，过 extract/restore 或非 normal 混合时颜色整体偏亮）。
 *
 * `out_changed` 回传「最终像素是否与 before 有任何字节不同」——全部被还原时
 * 调用方应整步撤回事务，不留空撤销步。
 *
 * 只支持 RGBA8：16 位/HDR 的 alpha 还原要按各自编码缩放（half 不能当整数乘），
 * 那是后续批次的事；尺寸/格式不符返回 false（调用方应中止，而不是留下半成品）。
 */
bool restore_transparent_lock(const ImageBuffer& before, ImageBuffer* after, bool* out_changed) {
  if (out_changed != nullptr) *out_changed = false;
  if (after == nullptr) return false;
  if (before.format != COMP_PIXEL_RGBA8 || after->format != COMP_PIXEL_RGBA8) return false;
  if (before.width != after->width || before.height != after->height) return false;
  if (before.empty() || after->empty()) return false;
  /* 仍是同一份缓冲 = 期间一次 writable_pixels() 都没调过（没有任何写入），
   * 没有可还原的东西，也避免读写同一块内存。 */
  if (before.pixels == after->pixels) return true;

  bool changed = false;
  for (int32_t y = 0; y < after->height; ++y) {
    const uint8_t* source = before.row(y);
    uint8_t* target = after->writable_row(y);
    for (int32_t x = 0; x < after->width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4u;
      const uint8_t old_alpha = source[offset + 3u];
      const uint8_t new_alpha = target[offset + 3u];
      if (old_alpha == 0u) {
        /* 全透明处：笔迹丢弃，alpha 与可能的垃圾 RGB 一起还原。 */
        std::memcpy(target + offset, source + offset, 4u);
      } else if (new_alpha == 0u) {
        /* 被抹成全透明：整份还原（锁透明时擦除不该删掉已有像素）。 */
        std::memcpy(target + offset, source + offset, 4u);
      } else {
        if (new_alpha != old_alpha) {
          for (size_t c = 0; c < 3u; ++c) {
            const unsigned value = target[offset + c];
            const unsigned scaled =
                (value * static_cast<unsigned>(old_alpha) + new_alpha / 2u) / new_alpha;
            target[offset + c] = static_cast<uint8_t>(scaled > 255u ? 255u : scaled);
          }
          target[offset + 3u] = old_alpha;
        }
      }
      if (!changed && std::memcmp(target + offset, source + offset, 4u) != 0) changed = true;
    }
  }
  if (out_changed != nullptr) *out_changed = changed;
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

namespace {

/* document_to_pixel 的连续版：同一套「归一化 -> 逆旋转 -> 翻转 -> 像素坐标」，
 * 但保留小数、不做范围夹取。document_to_pixel 在它之上取整并夹到缓冲内；
 * Progressive Blur 的渐变轴需要亚像素位置（一条跨画布的轴被取整会在缩放下抖动），
 * 直接走这个。
 *
 * 返回 false 只表示「几何不可用」（bounds 为空 / 源尺寸为 0），不表示点落在图层外。 */
bool map_document_to_pixel(const comp_rect& bounds, int32_t source_width, int32_t source_height,
                           float rotation, bool flip_x, bool flip_y, double doc_x, double doc_y,
                           double* out_x, double* out_y) {
  if (source_width <= 0 || source_height <= 0) return false;
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

  *out_x = (us + 1.0) * 0.5 * static_cast<double>(source_width) - 0.5;
  *out_y = (vs + 1.0) * 0.5 * static_cast<double>(source_height) - 0.5;
  return true;
}

}  // namespace

bool Layer::document_to_pixel_continuous(double doc_x, double doc_y, double* out_x,
                                         double* out_y) const {
  if (out_x == nullptr || out_y == nullptr) return false;
  return map_document_to_pixel(effective_bounds(), pixels.width, pixels.height, rotation, flip_x,
                               flip_y, doc_x, doc_y, out_x, out_y);
}

bool Layer::document_to_pixel(int32_t source_width, int32_t source_height, double doc_x,
                              double doc_y, int32_t* out_x, int32_t* out_y) const {
  if (out_x == nullptr || out_y == nullptr) return false;
  double sx = 0.0;
  double sy = 0.0;
  if (!map_document_to_pixel(effective_bounds(), source_width, source_height, rotation, flip_x,
                             flip_y, doc_x, doc_y, &sx, &sy)) {
    return false;
  }
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

bool Layer::pixel_to_document(int32_t source_width, int32_t source_height, int32_t px, int32_t py,
                              double* out_doc_x, double* out_doc_y) const {
  if (out_doc_x == nullptr || out_doc_y == nullptr) return false;
  if (source_width <= 0 || source_height <= 0) return false;
  if (px < 0 || py < 0 || px >= source_width || py >= source_height) return false;
  const comp_rect bounds = effective_bounds();
  if (bounds.w <= 0 || bounds.h <= 0) return false;

  /* 像素中心；与 document_to_pixel 的 (doc + 0.5) 对应。 */
  double us = (static_cast<double>(px) + 0.5) / static_cast<double>(source_width) * 2.0 - 1.0;
  double vs = (static_cast<double>(py) + 0.5) / static_cast<double>(source_height) * 2.0 - 1.0;

  /* document_to_pixel 是「逆旋转 → 翻转」，逆过来就是「翻转 → 正旋转」。 */
  if (flip_x) us = -us;
  if (flip_y) vs = -vs;

  double ud = us;
  double vd = vs;
  if (std::fabs(static_cast<double>(rotation)) > 1e-9) {
    constexpr double kPi = 3.14159265358979323846;
    const double rad = static_cast<double>(rotation) * kPi / 180.0;
    const double cos_t = std::cos(rad);
    const double sin_t = std::sin(rad);
    ud = us * cos_t - vs * sin_t;
    vd = us * sin_t + vs * cos_t;
  }

  *out_doc_x =
      (ud + 1.0) * 0.5 * static_cast<double>(bounds.w) + static_cast<double>(bounds.x) - 0.5;
  *out_doc_y =
      (vd + 1.0) * 0.5 * static_cast<double>(bounds.h) + static_cast<double>(bounds.y) - 0.5;
  return true;
}

}  // namespace compositor

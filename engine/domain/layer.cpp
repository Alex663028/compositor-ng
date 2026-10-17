// AI生成
#include "domain/layer.h"

#include <algorithm>
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

}  // namespace compositor

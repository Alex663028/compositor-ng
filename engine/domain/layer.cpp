// AI生成
#include "domain/layer.h"

#include <algorithm>
#include <cstring>

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
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* p = writable_row(y);
    for (int32_t x = 0; x < width; ++x) {
      p[0] = r;
      p[1] = g;
      p[2] = b;
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

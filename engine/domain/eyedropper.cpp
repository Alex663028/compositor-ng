// AI生成
#include "domain/eyedropper.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "compositing/compositor.h"
#include "domain/layer.h"

namespace compositor {
namespace {

bool fail(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
  return false;
}

/* 源缓冲统一落到 RGBA8（预乘）：16 位/HDR 走既有的 8 位工作副本，
 * 吸管拿到的就是屏幕上的颜色。返回的 ImageBuffer 可能只是借用。 */
bool to_rgba8(const ImageBuffer& source, ImageBuffer* scratch,
              const std::vector<uint8_t>** out_pixels, size_t* out_stride) {
  if (source.format == COMP_PIXEL_RGBA8) {
    *out_pixels = &source.pixel_data();
    *out_stride = source.stride();
    return true;
  }
  const bool ok = source.format == COMP_PIXEL_RGBA16F ? hdr_to_rgba8(source, scratch)
                                                     : deep_to_rgba8(source, scratch);
  if (!ok) return false;
  *out_pixels = &scratch->pixel_data();
  *out_stride = scratch->stride();
  return true;
}

}  // namespace

bool eyedropper_sample(const Document& document, comp_layer_id layer, double x, double y,
                       int32_t sample_size, int32_t source, float out_srgb[4],
                       std::string* error) {
  if (out_srgb == nullptr) return fail(error, "eyedropper: output is null");
  out_srgb[0] = out_srgb[1] = out_srgb[2] = out_srgb[3] = 0.0f;

  if (source != kEyeDropperLayer && source != kEyeDropperMerged) {
    return fail(error, "eyedropper: unknown source");
  }
  if (sample_size < 1 || sample_size > kMaxEyeDropperSample || (sample_size & 1) == 0) {
    return fail(error, "eyedropper: sample size must be odd in 1..101");
  }
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return fail(error, "eyedropper: coordinate is not finite");
  }

  const DocumentState& state = document.state();
  if (state.width <= 0 || state.height <= 0) return fail(error, "eyedropper: empty canvas");
  if (x < 0.0 || y < 0.0 || x >= static_cast<double>(state.width) ||
      y >= static_cast<double>(state.height)) {
    return fail(error, "eyedropper: coordinate lies outside the canvas");
  }

  ImageBuffer composite;
  ImageBuffer quantized;
  const Layer* target = nullptr;
  const std::vector<uint8_t>* pixels = nullptr;
  size_t stride = 0;

  if (source == kEyeDropperLayer) {
    target = document.find(layer);
    if (target == nullptr) return fail(error, "eyedropper: source layer was not found");
    if (target->is_group) return fail(error, "eyedropper: group layers have no pixels");
    if (target->pixels.empty()) return fail(error, "eyedropper: source layer has no pixels");
    if (!to_rgba8(target->pixels, &quantized, &pixels, &stride)) {
      return fail(error, "eyedropper: could not build an 8-bit working copy");
    }
  } else {
    std::string compose_error;
    if (!composite_document(document, &composite, &compose_error)) {
      return fail(error, "eyedropper: composite failed");
    }
    if (!to_rgba8(composite, &quantized, &pixels, &stride)) {
      return fail(error, "eyedropper: could not build an 8-bit working copy");
    }
  }

  const int32_t cx = static_cast<int32_t>(std::floor(x));
  const int32_t cy = static_cast<int32_t>(std::floor(y));
  const int32_t radius = sample_size / 2;

  double sum[4] = {0.0, 0.0, 0.0, 0.0};
  int32_t count = 0;
  for (int32_t dy = -radius; dy <= radius; ++dy) {
    const int32_t sy = cy + dy;
    if (sy < 0 || sy >= state.height) continue;
    for (int32_t dx = -radius; dx <= radius; ++dx) {
      const int32_t sx = cx + dx;
      if (sx < 0 || sx >= state.width) continue;

      int32_t px_x = sx;
      int32_t px_y = sy;
      if (target != nullptr &&
          !target->document_to_pixel(target->pixels.width, target->pixels.height,
                                     static_cast<double>(sx), static_cast<double>(sy), &px_x,
                                     &px_y)) {
        continue; /* 图层外的像素是透明的，不计入平均 */
      }

      const size_t offset = static_cast<size_t>(px_y) * stride + static_cast<size_t>(px_x) * 4u;
      if (offset + 4u > pixels->size()) continue;
      for (int c = 0; c < 4; ++c) sum[c] += static_cast<double>((*pixels)[offset + c]);
      ++count;
    }
  }

  if (count <= 0) return true; /* 整窗落在图层外：返回全 0（透明） */

  double premultiplied[4];
  for (int c = 0; c < 4; ++c) premultiplied[c] = sum[c] / static_cast<double>(count);
  const double alpha = premultiplied[3] / 255.0;
  out_srgb[3] = static_cast<float>(alpha);
  if (alpha <= 0.0) return true;
  for (int c = 0; c < 3; ++c) {
    const double straight = premultiplied[c] / 255.0 / alpha;
    out_srgb[c] = static_cast<float>(std::min(std::max(straight, 0.0), 1.0));
  }
  return true;
}

}  // namespace compositor

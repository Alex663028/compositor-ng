// AI生成
#include "domain/selection_pixels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/comp_geometry.h"

namespace compositor {
namespace {

bool fail(std::string* error, const char* text) {
  if (error != nullptr) *error = text;
  return false;
}

uint8_t to_byte(double value) {
  if (value <= 0.0) return 0;
  if (value >= 255.0) return 255;
  return static_cast<uint8_t>(std::lround(value));
}

struct MovedPixel {
  int32_t x = 0; /* 文档坐标（源） */
  int32_t y = 0;
  float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f}; /* 预乘，0..255 */
};

}  // namespace

bool selection_move_pixels(Layer* layer, int32_t canvas_width, int32_t canvas_height,
                           const SelectionState& selection, int32_t dx, int32_t dy,
                           bool duplicate, bool* out_changed, std::string* note,
                           std::string* error) {
  if (out_changed != nullptr) *out_changed = 0;
  if (layer == nullptr) return fail(error, "move pixels: layer is null");
  if (layer->is_group) return fail(error, "move pixels: group layers have no pixels");
  if (layer->pixels.empty()) return fail(error, "move pixels: layer has no pixels");
  if (layer->pixels.format != COMP_PIXEL_RGBA8) {
    return fail(error, "move pixels requires an 8-bit layer (16-bit/HDR not supported yet)");
  }
  if (std::fabs(static_cast<double>(layer->rotation)) > 1e-9 || layer->flip_x || layer->flip_y) {
    return fail(error, "move pixels requires an unrotated, unflipped layer");
  }
  if (canvas_width <= 0 || canvas_height <= 0) return fail(error, "move pixels: invalid canvas");
  if (std::abs(static_cast<int64_t>(dx)) > limits::kMaxTransformOrigin ||
      std::abs(static_cast<int64_t>(dy)) > limits::kMaxTransformOrigin) {
    return fail(error, "move pixels: offset out of range");
  }
  /* 原地搬移（且不复制）：没有可见变化，不产生撤销步。
   * 原地复制同样不产生可见变化——落点与源重合时「加上自己」只会把 alpha 叠满，
   * 那是数据损坏而不是复制，所以一并当作无操作。 */
  if (dx == 0 && dy == 0) return true;

  const comp_rect bounds = layer->effective_bounds();
  if (bounds.w != layer->pixels.width || bounds.h != layer->pixels.height) {
    return fail(error, "move pixels requires a 1:1 layer transform (no scaling)");
  }
  if (!layer->mask.empty() && (layer->mask.width != layer->pixels.width ||
                               layer->mask.height != layer->pixels.height)) {
    return fail(error, "move pixels: layer mask is not layer-sized");
  }

  /* 没有选区 / 显式空选区：无事可做，不产生撤销步。 */
  if (!selection.active) return true;

  std::vector<uint8_t> coverage;
  selection_effective_coverage(selection, &coverage);
  if (coverage.size() != static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height)) {
    return fail(error, "move pixels: selection grid does not match the canvas");
  }

  /* 只在「图层 ∩ 画布 ∩ 选区包围盒」里找真正要搬的像素。 */
  const comp_rect selection_rect = selection_bounds(selection);
  int64_t left = std::max<int64_t>(bounds.x, 0);
  int64_t top = std::max<int64_t>(bounds.y, 0);
  int64_t right = std::min<int64_t>(static_cast<int64_t>(bounds.x) + bounds.w, canvas_width);
  int64_t bottom = std::min<int64_t>(static_cast<int64_t>(bounds.y) + bounds.h, canvas_height);
  if (selection_rect.w > 0 && selection_rect.h > 0) {
    left = std::max<int64_t>(left, selection_rect.x);
    top = std::max<int64_t>(top, selection_rect.y);
    right = std::min<int64_t>(right, static_cast<int64_t>(selection_rect.x) + selection_rect.w);
    bottom = std::min<int64_t>(bottom, static_cast<int64_t>(selection_rect.y) + selection_rect.h);
  }
  if (left >= right || top >= bottom) return true;

  const std::vector<uint8_t>& pixels = layer->pixels.pixel_data();
  const size_t stride = layer->pixels.stride();

  std::vector<MovedPixel> moved;
  moved.reserve(static_cast<size_t>((right - left) * (bottom - top)));
  for (int64_t y = top; y < bottom; ++y) {
    const size_t row_offset = static_cast<size_t>(y - bounds.y) * stride;
    for (int64_t x = left; x < right; ++x) {
      const size_t cov_offset =
          static_cast<size_t>(y) * static_cast<size_t>(canvas_width) + static_cast<size_t>(x);
      const uint8_t cov = coverage[cov_offset];
      if (cov == 0) continue;

      const uint8_t* px = pixels.data() + row_offset +
                          static_cast<size_t>(x - bounds.x) * 4u;
      if (px[3] == 0) continue; /* 全透明像素没有可搬的内容 */

      MovedPixel item;
      item.x = static_cast<int32_t>(x);
      item.y = static_cast<int32_t>(y);
      const double factor = static_cast<double>(cov) / 255.0;
      for (int c = 0; c < 4; ++c) item.rgba[c] = static_cast<float>(px[c] * factor);
      moved.push_back(item);
    }
  }

  if (moved.empty()) return true; /* 选区里没有不透明像素 */

  /* 目标矩形的包围盒（文档坐标），用于决定是否需要扩层。 */
  int64_t dest_left = moved.front().x + dx;
  int64_t dest_top = moved.front().y + dy;
  int64_t dest_right = dest_left + 1;
  int64_t dest_bottom = dest_top + 1;
  for (const MovedPixel& item : moved) {
    const int64_t x = static_cast<int64_t>(item.x) + dx;
    const int64_t y = static_cast<int64_t>(item.y) + dy;
    dest_left = std::min(dest_left, x);
    dest_top = std::min(dest_top, y);
    dest_right = std::max(dest_right, x + 1);
    dest_bottom = std::max(dest_bottom, y + 1);
  }

  /* 画布外的落点直接丢弃（记 note），不把图层撑到画布之外。 */
  const int64_t clipped_left = std::max<int64_t>(dest_left, 0);
  const int64_t clipped_top = std::max<int64_t>(dest_top, 0);
  const int64_t clipped_right = std::min<int64_t>(dest_right, canvas_width);
  const int64_t clipped_bottom = std::min<int64_t>(dest_bottom, canvas_height);
  const bool dropped = clipped_left != dest_left || clipped_top != dest_top ||
                       clipped_right != dest_right || clipped_bottom != dest_bottom;
  const bool dest_visible = clipped_right > clipped_left && clipped_bottom > clipped_top;

  int64_t new_x = bounds.x;
  int64_t new_y = bounds.y;
  int64_t new_right = static_cast<int64_t>(bounds.x) + bounds.w;
  int64_t new_bottom = static_cast<int64_t>(bounds.y) + bounds.h;
  if (dest_visible) {
    new_x = std::min(new_x, clipped_left);
    new_y = std::min(new_y, clipped_top);
    new_right = std::max(new_right, clipped_right);
    new_bottom = std::max(new_bottom, clipped_bottom);
  }

  const int64_t new_width = new_right - new_x;
  const int64_t new_height = new_bottom - new_y;
  if (new_width <= 0 || new_height <= 0 || new_width > limits::kMaxSide ||
      new_height > limits::kMaxSide || new_width * new_height > limits::kMaxSurfacePixels) {
    return fail(error, "move pixels: expanded layer exceeds size limits");
  }

  const size_t nw = static_cast<size_t>(new_width);
  const size_t nh = static_cast<size_t>(new_height);
  std::vector<uint8_t> next(nw * nh * 4u, 0);

  /* 原像素按新原点落位（搬移与清源都在这份副本上做）。 */
  const int64_t copy_x = bounds.x - new_x;
  const int64_t copy_y = bounds.y - new_y;
  for (int32_t y = 0; y < layer->pixels.height; ++y) {
    std::memcpy(next.data() + (static_cast<size_t>(copy_y) + static_cast<size_t>(y)) * nw * 4u +
                    static_cast<size_t>(copy_x) * 4u,
                pixels.data() + static_cast<size_t>(y) * stride,
                static_cast<size_t>(layer->pixels.width) * 4u);
  }

  /* 清源（duplicate 时保留）与落点写入。 */
  size_t written = 0;
  for (const MovedPixel& item : moved) {
    const size_t src_index = static_cast<size_t>(item.y - new_y) * nw +
                             static_cast<size_t>(item.x - new_x);
    uint8_t* src = next.data() + src_index * 4u;
    if (!duplicate) {
      for (int c = 0; c < 4; ++c) src[c] = to_byte(static_cast<double>(src[c]) - item.rgba[c]);
    }

    const int32_t target_x = item.x + dx;
    const int32_t target_y = item.y + dy;
    if (target_x < 0 || target_y < 0 || target_x >= canvas_width || target_y >= canvas_height) {
      continue;
    }
    const size_t dst_index = static_cast<size_t>(target_y - new_y) * nw +
                             static_cast<size_t>(target_x - new_x);
    uint8_t* dst = next.data() + dst_index * 4u;
    for (int c = 0; c < 4; ++c) {
      dst[c] = to_byte(static_cast<double>(dst[c]) + static_cast<double>(item.rgba[c]));
    }
    ++written;
  }

  /* 提交：像素矩形与 transform 一起更新。 */
  layer->pixels.resize(static_cast<int32_t>(new_width), static_cast<int32_t>(new_height),
                       COMP_PIXEL_RGBA8);
  std::memcpy(layer->pixels.writable_pixels().data(), next.data(), next.size());
  layer->transform.x = static_cast<int32_t>(new_x);
  layer->transform.y = static_cast<int32_t>(new_y);
  layer->transform.w = static_cast<int32_t>(new_width);
  layer->transform.h = static_cast<int32_t>(new_height);

  /* 蒙版与像素矩形同步：原蒙版内容平移到新原点，新增区域填 255（未蒙）。 */
  if (!layer->mask.empty()) {
    const int32_t old_width = layer->mask.width;
    const int32_t old_height = layer->mask.height;
    std::vector<uint8_t> old_mask(layer->mask.pixel_data());
    std::vector<uint8_t> new_mask(nw * nh, 255);
    for (int32_t y = 0; y < old_height; ++y) {
      std::memcpy(new_mask.data() + (static_cast<size_t>(copy_y) + static_cast<size_t>(y)) * nw +
                      static_cast<size_t>(copy_x),
                  old_mask.data() + static_cast<size_t>(y) * static_cast<size_t>(old_width),
                  static_cast<size_t>(old_width));
    }
    layer->mask.width = static_cast<int32_t>(new_width);
    layer->mask.height = static_cast<int32_t>(new_height);
    layer->mask.pixels = std::make_shared<std::vector<uint8_t>>(std::move(new_mask));
  }

  if (note != nullptr) {
    if (new_width != bounds.w || new_height != bounds.h) {
      *note += "layer pixels were expanded to hold the moved content; ";
    }
    if (dropped || written < moved.size()) {
      *note += "pixels moved outside the canvas were dropped; ";
    }
    if (duplicate) *note += "source pixels were kept (duplicate); ";
  }
  if (out_changed != nullptr) *out_changed = 1;
  return true;
}

}  // namespace compositor

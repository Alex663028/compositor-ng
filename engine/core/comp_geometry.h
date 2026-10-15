// AI生成
/*
 * comp_geometry.h — 几何工具（header-only）。
 *
 * M1 只覆盖矩形/尺寸的基础运算，满足画布裁剪、图层包围盒与选区求交的需要。
 * 有意不引入矩阵库：仿射变换在 M1 由 CPU 合成器以最简形式处理（平移+翻转），
 * 完整仿射（旋转/透视）归 M2 的变形模块。
 */
#ifndef COMPOSITOR_CORE_COMP_GEOMETRY_H
#define COMPOSITOR_CORE_COMP_GEOMETRY_H

#include <algorithm>

#include "compositor/comp_core.h"

namespace compositor {

inline comp_rect make_rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  return comp_rect{x, y, w, h};
}

inline comp_size make_size(int32_t w, int32_t h) { return comp_size{w, h}; }

inline comp_rect intersection(const comp_rect& a, const comp_rect& b) {
  const int32_t x0 = std::max(a.x, b.x);
  const int32_t y0 = std::max(a.y, b.y);
  const int32_t x1 = std::min(a.x + a.w, b.x + b.w);
  const int32_t y1 = std::min(a.y + a.h, b.y + b.h);
  if (x1 <= x0 || y1 <= y0) return comp_rect{x0, y0, 0, 0};
  return comp_rect{x0, y0, x1 - x0, y1 - y0};
}

inline bool is_empty(const comp_rect& r) { return r.w <= 0 || r.h <= 0; }

inline int64_t area(const comp_rect& r) {
  if (is_empty(r)) return 0;
  return static_cast<int64_t>(r.w) * static_cast<int64_t>(r.h);
}

inline bool contains(const comp_rect& outer, const comp_rect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y &&
         inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

/* 按画布尺寸裁剪出合法的图层包围盒。 */
inline comp_rect clamp_to_canvas(const comp_rect& r, comp_size canvas) {
  return intersection(r, comp_rect{0, 0, canvas.width, canvas.height});
}

inline int64_t pixel_count(comp_size s) {
  return static_cast<int64_t>(s.width) * static_cast<int64_t>(s.height);
}

/* 校验画布尺寸是否落在 .comp 规范允许范围内。 */
inline bool is_valid_canvas(comp_size s) {
  constexpr int32_t kMaxEdge = 30000;
  constexpr int64_t kMaxPixels = 100000000LL; /* 1 亿像素 */
  if (s.width <= 0 || s.height <= 0) return false;
  if (s.width > kMaxEdge || s.height > kMaxEdge) return false;
  return pixel_count(s) <= kMaxPixels;
}

inline int32_t bytes_per_pixel(comp_pixel_format fmt) {
  switch (fmt) {
    case COMP_PIXEL_RGBA8: return 4;
    case COMP_PIXEL_RGBA16: return 8;
    case COMP_PIXEL_RGBA16F: return 8;
    default: return 0;
  }
}

}  // namespace compositor

#endif /* COMPOSITOR_CORE_COMP_GEOMETRY_H */

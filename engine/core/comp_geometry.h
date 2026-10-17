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
#include <cmath>

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

/* ------------------------------------------------------------ 文档尺寸上限
 *
 * 单一真源：引擎内所有「多大多小算合法」的判据都读这里的常量。此前
 * 30000 / 1e8 三处各写一份（is_valid_canvas、resample.cpp 的 build_layer_unit、
 * comp_file.cpp 的写侧钳制），任何一处漏改就变成「写进去能存、读出来渲不了」
 * 的契约漂移（见 AUDIT4 §2）。
 *
 * 取值对齐上游 Compositor/Document/DocumentLimits.swift：
 *   maxSide = 30_000；maxSurfacePixels = 200_000_000
 *
 * 与上游的一处**有意分歧**：上游 maxSurfacePixels 是 2 亿，本引擎是 1 亿。
 * 这是此前就存在的口径（tests/test_core.cpp 已按 1 亿钉住），不在本轮改动范围；
 * 记在这里是为了让下一个读到这里的人知道它是已知偏差，而不是漏抄。
 */
namespace limits {

/* 最长边（像素）：画布 / 图层 / 蒙版 / 任何生成表面。 */
inline constexpr int32_t kMaxSide = 30000;

/* 单个表面像素数上限：RGBA8 下约 400 MB。 */
inline constexpr int64_t kMaxSurfacePixels = 100000000LL; /* 1 亿像素（上游为 2 亿） */

/* transform 尺寸分量上限（像素）。 */
inline constexpr int64_t kMaxTransformSize = 300000;   /* 上游 LayerTransform.isValid */

/* transform 原点绝对值上限（像素）。 */
inline constexpr int64_t kMaxTransformOrigin = 1000000;

}  // namespace limits

/* 校验画布尺寸是否落在 .comp 规范允许范围内。 */
inline bool is_valid_canvas(comp_size s) {
  if (s.width <= 0 || s.height <= 0) return false;
  if (s.width > limits::kMaxSide || s.height > limits::kMaxSide) return false;
  return pixel_count(s) <= limits::kMaxSurfacePixels;
}

/* ------------------------------------------------------------ transform 有效性
 *
 * 对齐上游 LayerTransform.isValid（Compositor/Document/LayerTransform.swift:27）：
 *
 *   [origin.x, origin.y, size.width, size.height, rotation].allSatisfy(\.isFinite)
 *       && (1...300_000).contains(size.width) && (1...300_000).contains(size.height)
 *       && abs(origin.x) <= 1_000_000 && abs(origin.y) <= 1_000_000
 *
 * 上游是「写入前校验 + 不合法则回退原值」（EditorSession.swift:506 `guard moved.isValid
 * else { continue }`、LayerTransform.swift:213 `return result.isValid ? result : original`），
 * 即**拒绝非法值**，不是钳制。本引擎的 ABI 层此前零校验，直接把任意 comp_rect 写进
 * 图层，于是越界值能存盘、能往返 .comp、到合成时才炸——宿主拿不到任何达因。
 *
 * 这里给出与上游同口径的判定，供 ABI 入口把关；.comp 读写侧的**钳制**是另一回事
 * （manifest 是不可信的交换格式，只能钳不能拒），两者各自保留，边界常量同源。
 */
inline bool is_valid_transform(const comp_rect& r, float rotation) {
  /* w/h <= 0 是引擎的「按像素尺寸回落」语义（见 Layer::effective_bounds），
   * 对应上游尺寸未指定的情形，不是非法值，故此处只要求不越上界。 */
  if (r.w > static_cast<int32_t>(limits::kMaxTransformSize)) return false;
  if (r.h > static_cast<int32_t>(limits::kMaxTransformSize)) return false;
  if (r.x > static_cast<int32_t>(limits::kMaxTransformOrigin)) return false;
  if (r.x < -static_cast<int32_t>(limits::kMaxTransformOrigin)) return false;
  if (r.y > static_cast<int32_t>(limits::kMaxTransformOrigin)) return false;
  if (r.y < -static_cast<int32_t>(limits::kMaxTransformOrigin)) return false;
  /* 上游 allSatisfy(\.isFinite) 同样覆盖 rotation：NaN/inf 会让 cos/sin 产出 NaN，
     之后所有比较都为 false，图层静默渲成全透明且不报任何错（实测 inf 即如此）。 */
  return std::isfinite(rotation);
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

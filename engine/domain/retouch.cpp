// AI生成
/*
 * retouch.cpp — Clone Stamp / Smudge / Liquify 的像素算法（见 retouch.h 的契约）。
 *
 * 三个函数都只读入「已经翻译好的图层像素空间参数」，不查文档、不开事务。
 * 算法逐条对齐上游 CloneStamp.swift 与 SmudgeLiquify.swift；注释里给出对应
 * 的那一步，便于把本仓行为与 Swift 真源逐行对上。
 */
#include "domain/retouch.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace compositor {
namespace {

/* 上游 `.rounded()`（半边远离零），与 crop_snap_frame 的取整口径一致。 */
inline int32_t round_px(float v) { return static_cast<int32_t>(std::lround(v)); }

inline uint8_t to_byte(float v) {
  if (!(v > 0.0f)) return 0u; /* 同时挡住 NaN */
  if (v >= 1.0f) return 255u;
  return static_cast<uint8_t>(std::lround(v * 255.0f));
}

inline float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

/*
 * 上游 SmudgeLiquify.swift 的 `weight(u)`：
 *   u <= hardness -> 1；否则 t = (1-u)/(1-hardness)，smoothstep(t)。
 * hardness 由调用方夹到 [0, 0.98]，因此 (1-hardness) 不会为 0。
 */
float warp_weight(float u, float hardness) {
  if (u <= hardness) return 1.0f;
  if (u >= 1.0f) return 0.0f;
  const float t = (1.0f - u) / (1.0f - hardness);
  return t * t * (3.0f - 2.0f * t);
}

/*
 * 预乘 RGBA8 的双线性采样。x/y 先夹到像素中心范围，避免 NaN/越界读。
 * 预乘空间插值才不会在 alpha 边缘出现暗边（见 retouch.h 文件头）。
 */
void sample_bilinear4(const uint8_t* base, int32_t width, int32_t height, size_t stride,
                      float x, float y, float out[4]) {
  if (width <= 0 || height <= 0) {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    return;
  }
  x = std::min(std::max(x, 0.0f), static_cast<float>(width - 1));
  y = std::min(std::max(y, 0.0f), static_cast<float>(height - 1));
  const int32_t ix = std::min(static_cast<int32_t>(std::floor(x)), width - 1);
  const int32_t iy = std::min(static_cast<int32_t>(std::floor(y)), height - 1);
  const int32_t ix1 = std::min(ix + 1, width - 1);
  const int32_t iy1 = std::min(iy + 1, height - 1);
  const float fx = x - static_cast<float>(ix);
  const float fy = y - static_cast<float>(iy);

  const uint8_t* p00 = base + static_cast<size_t>(iy) * stride + static_cast<size_t>(ix) * 4u;
  const uint8_t* p10 = base + static_cast<size_t>(iy) * stride + static_cast<size_t>(ix1) * 4u;
  const uint8_t* p01 = base + static_cast<size_t>(iy1) * stride + static_cast<size_t>(ix) * 4u;
  const uint8_t* p11 = base + static_cast<size_t>(iy1) * stride + static_cast<size_t>(ix1) * 4u;
  for (int c = 0; c < 4; ++c) {
    const float top = static_cast<float>(p00[c]) * (1.0f - fx) + static_cast<float>(p10[c]) * fx;
    const float bottom = static_cast<float>(p01[c]) * (1.0f - fx) + static_cast<float>(p11[c]) * fx;
    out[c] = (top * (1.0f - fy) + bottom * fy) / 255.0f;
  }
}

/*
 * 折线（含顶点本身）到采样点的最近距离平方。顶点也参与取最小值，因此
 * 单点折线 = 以该点为圆心的一个 dab——宿主「点一下」也应该落下笔迹。
 */
float polyline_distance2(const float* points, int32_t count, float px, float py) {
  float best = 1e30f;
  for (int32_t i = 0; i < count; ++i) {
    const float vx = points[i * 2] - px;
    const float vy = points[i * 2 + 1] - py;
    best = std::min(best, vx * vx + vy * vy);
  }
  for (int32_t i = 0; i + 1 < count; ++i) {
    const float ax = points[i * 2], ay = points[i * 2 + 1];
    const float bx = points[(i + 1) * 2], by = points[(i + 1) * 2 + 1];
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
    t = std::min(std::max(t, 0.0f), 1.0f);
    const float ex = px - (ax + dx * t), ey = py - (ay + dy * t);
    best = std::min(best, ex * ex + ey * ey);
  }
  return best;
}

/* 折线包围盒（按 radius 外扩后夹到图像内）；返回 false 表示完全在图像外。 */
bool polyline_bounds(const float* points, int32_t count, float radius, int32_t width,
                     int32_t height, int32_t* out_x0, int32_t* out_y0, int32_t* out_x1,
                     int32_t* out_y1) {
  float min_x = points[0], max_x = points[0], min_y = points[1], max_y = points[1];
  for (int32_t i = 1; i < count; ++i) {
    min_x = std::min(min_x, points[i * 2]);
    max_x = std::max(max_x, points[i * 2]);
    min_y = std::min(min_y, points[i * 2 + 1]);
    max_y = std::max(max_y, points[i * 2 + 1]);
  }
  int32_t x0 = static_cast<int32_t>(std::floor(min_x - radius));
  int32_t y0 = static_cast<int32_t>(std::floor(min_y - radius));
  int32_t x1 = static_cast<int32_t>(std::ceil(max_x + radius));
  int32_t y1 = static_cast<int32_t>(std::ceil(max_y + radius));
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, width - 1);
  y1 = std::min(y1, height - 1);
  if (x0 > x1 || y0 > y1) return false;
  *out_x0 = x0;
  *out_y0 = y0;
  *out_x1 = x1;
  *out_y1 = y1;
  return true;
}

/*
 * warp 的公共几何：直径 -> 半径，hardness 夹到上游的 0.98 上限。
 * 上游 `diameter = max(2, settings.diameter)`；参数闸已保证 diameter > 0。
 */
struct WarpKernel {
  int32_t radius = 1;
  float hardness = 0.0f;
  float radius_f = 1.0f;

  WarpKernel(float diameter, float hardness_in) {
    const float d = std::max(2.0f, diameter);
    radius = static_cast<int32_t>(std::ceil(d * 0.5f));
    radius_f = d * 0.5f;
    hardness = std::min(std::max(hardness_in, 0.0f), 0.98f);
  }
};

/* 折线按上游间距重采样成 dab 位置（首点之外）。 */
std::vector<float> resample_dabs(const float* points, int32_t count, float spacing) {
  std::vector<float> dabs;
  if (count < 2) return dabs;
  for (int32_t i = 0; i + 1 < count; ++i) {
    const float ax = points[i * 2], ay = points[i * 2 + 1];
    const float bx = points[(i + 1) * 2], by = points[(i + 1) * 2 + 1];
    const float dx = bx - ax, dy = by - ay;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (!(distance > 0.0f)) continue;
    const int32_t steps = static_cast<int32_t>(std::ceil(distance / spacing));
    for (int32_t s = 1; s <= steps; ++s) {
      const float t = static_cast<float>(s) / static_cast<float>(steps);
      dabs.push_back(ax + dx * t);
      dabs.push_back(ay + dy * t);
    }
  }
  return dabs;
}

}  // namespace

/* ============================================================ Clone Stamp */

bool clone_stamp_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                       const uint8_t* source, int32_t offset_x, int32_t offset_y,
                       const float* points, int32_t point_count, float diameter,
                       float hardness, float coverage_peak) {
  if (rgba == nullptr || source == nullptr || points == nullptr) return true;
  if (width <= 0 || height <= 0 || point_count <= 0) return true;
  if (!(coverage_peak > 0.0f)) return true;

  const float d = std::max(diameter, 1.0f);
  const float radius = d * 0.5f;
  const float hard = std::min(std::max(hardness, 0.0f), 1.0f);
  const float inner = radius * hard;
  const float feather = radius - inner;
  const float peak = std::min(coverage_peak, 1.0f);

  auto coverage_at = [&](float px, float py) -> float {
    const float d2 = polyline_distance2(points, point_count, px, py);
    if (d2 > radius * radius) return 0.0f;
    const float dist = std::sqrt(d2);
    if (dist <= inner || feather <= 0.0f) return peak;
    const float t = 1.0f - (dist - inner) / feather;
    return peak * t * t * (3.0f - 2.0f * t);
  };

  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (!polyline_bounds(points, point_count, radius, width, height, &x0, &y0, &x1, &y1)) {
    return true;
  }

  /*
   * 源 = 快照（一笔开始时取好）。越界跳过而不是取透明：上游在源越界时同样
   * 「没有可画的像素」，落透明会把图层擦出洞，语义完全不同。
   */
  for (int32_t y = y0; y <= y1; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    for (int32_t x = x0; x <= x1; ++x) {
      const float cov = coverage_at(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
      if (cov <= 0.0f) continue;
      const int32_t sx = x + offset_x;
      const int32_t sy = y + offset_y;
      if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;

      const uint8_t* src = source + static_cast<size_t>(sy) * stride + static_cast<size_t>(sx) * 4u;
      uint8_t* dst = row + static_cast<size_t>(x) * 4u;
      const float sa = (static_cast<float>(src[3]) / 255.0f) * cov;
      const float keep = 1.0f - sa;
      for (int c = 0; c < 3; ++c) {
        const float sc = (static_cast<float>(src[c]) / 255.0f) * cov;
        const float dc = static_cast<float>(dst[c]) / 255.0f;
        dst[c] = to_byte(sc + dc * keep);
      }
      const float da = static_cast<float>(dst[3]) / 255.0f;
      dst[3] = to_byte(sa + da * keep);
    }
  }
  return true;
}

/* ============================================================ Smudge */

bool smudge_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                  const float* points, int32_t point_count, float diameter, float hardness,
                  float strength) {
  if (rgba == nullptr || points == nullptr) return true;
  if (width <= 0 || height <= 0 || point_count <= 0) return true;
  if (!(strength > 0.0f)) return true;

  const WarpKernel kernel(diameter, hardness);
  const int32_t r = kernel.radius;
  const int32_t side = 2 * r + 1;
  std::vector<float> carried(static_cast<size_t>(side) * static_cast<size_t>(side) * 4u, 0.0f);

  /*
   * 上游 `pickUp`：以首点为圆心把 under 读进 carried，越界保持 0。
   * 首点只拾色，不落笔——之后每个移动采样才产生 dab。
   */
  const int32_t px0 = round_px(points[0]);
  const int32_t py0 = round_px(points[1]);
  for (int32_t dy = -r; dy <= r; ++dy) {
    const int32_t sy = py0 + dy;
    if (sy < 0 || sy >= height) continue;
    for (int32_t dx = -r; dx <= r; ++dx) {
      const int32_t sx = px0 + dx;
      if (sx < 0 || sx >= width) continue;
      const uint8_t* src = rgba + static_cast<size_t>(sy) * stride + static_cast<size_t>(sx) * 4u;
      float* slot = carried.data() +
                    (static_cast<size_t>(dy + r) * static_cast<size_t>(side) +
                     static_cast<size_t>(dx + r)) * 4u;
      for (int c = 0; c < 4; ++c) slot[c] = static_cast<float>(src[c]) / 255.0f;
    }
  }

  /* 上游间距：smudge `max(1, diameter * 0.005)`。 */
  const std::vector<float> dabs = resample_dabs(points, point_count,
                                                std::max(1.0f, diameter * 0.005f));
  for (size_t dab = 0; dab + 1 < dabs.size(); dab += 2) {
    const int32_t cx = round_px(dabs[dab]);
    const int32_t cy = round_px(dabs[dab + 1]);
    for (int32_t dy = -r; dy <= r; ++dy) {
      const int32_t y = cy + dy;
      if (y < 0 || y >= height) continue;
      uint8_t* row = rgba + static_cast<size_t>(y) * stride;
      for (int32_t dx = -r; dx <= r; ++dx) {
        const int32_t x = cx + dx;
        if (x < 0 || x >= width) continue;
        const float u = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / kernel.radius_f;
        const float w = warp_weight(u, kernel.hardness);
        if (w <= 0.0f) continue;

        uint8_t* dst = row + static_cast<size_t>(x) * 4u;
        float* slot = carried.data() +
                      (static_cast<size_t>(dy + r) * static_cast<size_t>(side) +
                       static_cast<size_t>(dx + r)) * 4u;
        const float mix = w * strength;
        for (int c = 0; c < 4; ++c) {
          const float under = static_cast<float>(dst[c]) / 255.0f;
          /* 上游：painted = under + (carried - under) * weight * strength。 */
          const float painted = under + (slot[c] - under) * mix;
          const float clamped = clamp01(painted);
          dst[c] = to_byte(clamped);
          slot[c] = clamped;
        }
      }
    }
  }
  return true;
}

/* ============================================================ Liquify (push) */

bool liquify_push_paint(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                        const float* points, int32_t point_count, float diameter, float hardness,
                        float strength) {
  if (rgba == nullptr || points == nullptr) return true;
  if (width <= 0 || height <= 0 || point_count < 2) return true;
  if (!(strength > 0.0f)) return true;

  const WarpKernel kernel(diameter, hardness);
  const int32_t r = kernel.radius;

  /* 上游间距：liquify `max(1, diameter * 0.025)`。 */
  const std::vector<float> dabs = resample_dabs(points, point_count,
                                                std::max(1.0f, diameter * 0.025f));
  float prev_x = points[0], prev_y = points[1];
  for (size_t dab = 0; dab + 1 < dabs.size(); dab += 2) {
    const float to_x = dabs[dab], to_y = dabs[dab + 1];
    /* 上游 `move = (to - from) * strength`，from 是上一个 dab 位置。 */
    const float move_x = (to_x - prev_x) * strength;
    const float move_y = (to_y - prev_y) * strength;
    prev_x = to_x;
    prev_y = to_y;

    const int32_t cx = round_px(to_x);
    const int32_t cy = round_px(to_y);
    /* 位移余量：dab 覆盖范围内可能被采到的旧像素都要进 scratch。 */
    const int32_t margin = static_cast<int32_t>(
                               std::ceil(std::max(std::fabs(move_x), std::fabs(move_y)))) +
                           2;
    int32_t x0 = std::max(cx - r - margin, 0);
    int32_t y0 = std::max(cy - r - margin, 0);
    int32_t x1 = std::min(cx + r + margin, width - 1);
    int32_t y1 = std::min(cy + r + margin, height - 1);
    if (x0 > x1 || y0 > y1) continue;
    const int32_t cw = x1 - x0 + 1;
    const int32_t ch = y1 - y0 + 1;
    if (cw < 2 || ch < 2) continue;

    std::vector<uint8_t> scratch(static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4u);
    for (int32_t y = 0; y < ch; ++y) {
      std::memcpy(scratch.data() + static_cast<size_t>(y) * static_cast<size_t>(cw) * 4u,
                  rgba + static_cast<size_t>(y0 + y) * stride + static_cast<size_t>(x0) * 4u,
                  static_cast<size_t>(cw) * 4u);
    }

    for (int32_t dy = -r; dy <= r; ++dy) {
      const int32_t y = cy + dy;
      if (y < y0 || y > y1) continue;
      uint8_t* row = rgba + static_cast<size_t>(y) * stride;
      for (int32_t dx = -r; dx <= r; ++dx) {
        const int32_t x = cx + dx;
        if (x < x0 || x > x1) continue;
        const float u = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / kernel.radius_f;
        const float w = warp_weight(u, kernel.hardness);
        if (w <= 0.0f) continue;

        /* 从 scratch（本 dab 之前的像素）采样，避免自反馈。 */
        float sampled[4];
        sample_bilinear4(scratch.data(), cw, ch, static_cast<size_t>(cw) * 4u,
                         static_cast<float>(x - x0) - move_x * w,
                         static_cast<float>(y - y0) - move_y * w, sampled);
        uint8_t* dst = row + static_cast<size_t>(x) * 4u;
        for (int c = 0; c < 4; ++c) dst[c] = to_byte(sampled[c]);
      }
    }
  }
  return true;
}

}  // namespace compositor

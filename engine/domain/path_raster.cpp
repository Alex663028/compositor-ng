// AI生成
#include "domain/path_raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace compositor {

namespace {

constexpr double kPi = 3.14159265358979323846;

/* 有向边：按 y 排序存放（y0 < y1），dir 记录原始走向。
 * 非零缠绕需要方向，只按「穿越奇偶」计数会把反向子路径的洞填实。 */
struct Edge {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  int dir = 1;
};

void add_edge(double x0, double y0, double x1, double y1, std::vector<Edge>* edges) {
  if (y0 == y1) return; /* 水平边不产生穿越 */
  if (y0 < y1) {
    edges->push_back(Edge{x0, y0, x1, y1, +1});
  } else {
    edges->push_back(Edge{x1, y1, x0, y0, -1});
  }
}

void add_contour_edges(const PathContour& contour, std::vector<Edge>* edges) {
  const size_t count = contour.size() / 2;
  if (count < 3) return; /* 少于 3 点没有面积（与「退化形状」同口径） */
  for (size_t i = 0; i < count; ++i) {
    const size_t j = (i + 1) % count;
    add_edge(contour[2 * i], contour[2 * i + 1], contour[2 * j], contour[2 * j + 1], edges);
  }
}

/* 把 [a, b) 这段水平区间按解析覆盖率累加进一行（uint32 存样本票数，
 * 4 个子样本时一像素最多 1020，不会溢出）。 */
void add_horizontal_span(double a, double b, std::vector<uint32_t>* row, int32_t width) {
  if (!(a < b)) return;
  if (b <= 0.0 || a >= static_cast<double>(width)) return;
  if (a < 0.0) a = 0.0;
  if (b > static_cast<double>(width)) b = static_cast<double>(width);
  int32_t first = static_cast<int32_t>(std::floor(a));
  int32_t last = static_cast<int32_t>(std::ceil(b)) - 1;
  if (first < 0) first = 0;
  if (last > width - 1) last = width - 1;
  for (int32_t x = first; x <= last; ++x) {
    const double left = std::max(a, static_cast<double>(x));
    const double right = std::min(b, static_cast<double>(x + 1));
    if (right <= left) continue;
    /* 整像素区间直接加 255，只有两端的不完整像素才走取整：
     * 一次采样最多 2 次 lround，满画布形状不会卡在逐像素取整上。 */
    const uint32_t value =
        (right - left >= 1.0) ? 255u
                              : static_cast<uint32_t>(std::lround((right - left) * 255.0));
    (*row)[static_cast<size_t>(x)] += value;
  }
}

/* 圆角矩形的一个角：从 (from) 经控制点 (c1, c2) 到 (to)。 */
void add_corner(double from_x, double from_y, double c1x, double c1y, double c2x, double c2y,
                double to_x, double to_y, PathContour* contour) {
  /* 上一段已经落在 from 上（或轮廓起点），这里只追加终点。 */
  path_flatten_cubic(from_x, from_y, c1x, c1y, c2x, c2y, to_x, to_y, 0, 65536u, contour);
}

}  // namespace

void path_add_rect_contour(double x0, double y0, double x1, double y1, PathContours* contours) {
  const double left = std::min(x0, x1);
  const double right = std::max(x0, x1);
  const double top = std::min(y0, y1);
  const double bottom = std::max(y0, y1);
  contours->push_back(PathContour{static_cast<float>(left), static_cast<float>(top),
                                  static_cast<float>(right), static_cast<float>(top),
                                  static_cast<float>(right), static_cast<float>(bottom),
                                  static_cast<float>(left), static_cast<float>(bottom)});
}

/*
 * 椭圆：按弦高误差取段数。半径 r 的圆用 n 段折线近似时，最大径向误差
 * 约 r * (1 - cos(π/n))；令它 <= 0.05 像素得 n >= π * sqrt(r / 0.025)
 * （代入展开后即 2π·sqrt(2.5r)）。下限 32 段保证小椭圆也不会显棱角，
 * 上限 8192 段挡住 30000 像素的画布。
 */
void path_add_ellipse_contour(double x0, double y0, double x1, double y1,
                              PathContours* contours) {
  const double left = std::min(x0, x1);
  const double right = std::max(x0, x1);
  const double top = std::min(y0, y1);
  const double bottom = std::max(y0, y1);
  const double cx = (left + right) * 0.5;
  const double cy = (top + bottom) * 0.5;
  const double rx = (right - left) * 0.5;
  const double ry = (bottom - top) * 0.5;
  const double radius = std::max(rx, ry);
  double requested = std::ceil(2.0 * kPi * std::sqrt(2.5 * std::max(radius, 1e-6)));
  int segments = 32;
  if (requested > static_cast<double>(segments)) segments = static_cast<int>(requested);
  if (segments > 8192) segments = 8192;

  PathContour contour;
  contour.reserve(static_cast<size_t>(segments) * 2);
  for (int i = 0; i < segments; ++i) {
    const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(segments);
    contour.push_back(static_cast<float>(cx + rx * std::cos(angle)));
    contour.push_back(static_cast<float>(cy + ry * std::sin(angle)));
  }
  contours->push_back(std::move(contour));
}

void path_add_rounded_rect_contour(double x0, double y0, double x1, double y1, double radius,
                                   PathContours* contours) {
  const double left = std::min(x0, x1);
  const double right = std::max(x0, x1);
  const double top = std::min(y0, y1);
  const double bottom = std::max(y0, y1);
  const double half = std::min(right - left, bottom - top) * 0.5;
  double r = radius;
  if (!(r > 0.0)) r = 0.0; /* NaN / 负数一律按直角处理（参数闸另有拒绝） */
  if (r > half) r = half;  /* 上游 ShapeTool 同款夹取：不超过短边一半 */
  if (r <= 0.0) {
    path_add_rect_contour(left, top, right, bottom, contours);
    return;
  }
  /* 三次贝塞尔逼近 90° 圆弧的控制点系数（4/3·tan(45°/2) = 0.5522847498）。 */
  const double k = r * 0.5522847498307936;
  PathContour contour;
  contour.reserve(64);
  contour.push_back(static_cast<float>(left + r));
  contour.push_back(static_cast<float>(top));
  contour.push_back(static_cast<float>(right - r));
  contour.push_back(static_cast<float>(top));
  add_corner(right - r, top, right - r + k, top, right, top + r - k, right, top + r, &contour);
  contour.push_back(static_cast<float>(right));
  contour.push_back(static_cast<float>(bottom - r));
  add_corner(right, bottom - r, right, bottom - r + k, right - r + k, bottom, right - r, bottom,
             &contour);
  contour.push_back(static_cast<float>(left + r));
  contour.push_back(static_cast<float>(bottom));
  add_corner(left + r, bottom, left + r - k, bottom, left, bottom - r + k, left, bottom - r,
             &contour);
  contour.push_back(static_cast<float>(left));
  contour.push_back(static_cast<float>(top + r));
  add_corner(left, top + r, left, top + r - k, left + r - k, top, left + r, top, &contour);
  contours->push_back(std::move(contour));
}

/* 自适应细分（de Casteljau 中点分裂）。控制点二阶差分足够小时，
 * 曲线与控制多边形都塌到直线段上，直接收尾。 */
void path_flatten_cubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                        double y3, int depth, size_t max_points, PathContour* contour) {
  const double d1x = std::fabs(x0 - 2.0 * x1 + x2);
  const double d1y = std::fabs(y0 - 2.0 * y1 + y2);
  const double d2x = std::fabs(x1 - 2.0 * x2 + x3);
  const double d2y = std::fabs(y1 - 2.0 * y2 + y3);
  if (depth >= kPathFlattenMaxDepth || contour->size() / 2 >= max_points ||
      (d1x + d1y <= kPathFlattenTolerance && d2x + d2y <= kPathFlattenTolerance)) {
    contour->push_back(static_cast<float>(x3));
    contour->push_back(static_cast<float>(y3));
    return;
  }
  const double x01 = (x0 + x1) * 0.5;
  const double y01 = (y0 + y1) * 0.5;
  const double x12 = (x1 + x2) * 0.5;
  const double y12 = (y1 + y2) * 0.5;
  const double x23 = (x2 + x3) * 0.5;
  const double y23 = (y2 + y3) * 0.5;
  const double x012 = (x01 + x12) * 0.5;
  const double y012 = (y01 + y12) * 0.5;
  const double x123 = (x12 + x23) * 0.5;
  const double y123 = (y12 + y23) * 0.5;
  const double xm = (x012 + x123) * 0.5;
  const double ym = (y012 + y123) * 0.5;
  path_flatten_cubic(x0, y0, x01, y01, x012, y012, xm, ym, depth + 1, max_points, contour);
  path_flatten_cubic(xm, ym, x123, y123, x23, y23, x3, y3, depth + 1, max_points, contour);
}

/*
 * 形状栅格化：非零缠绕 + 4 行超采样 + 水平方向解析覆盖率。
 *
 * 与上游 CGPath.fillPath(using: .winding) 对齐的地方是**填充规则**：
 * 反向子路径挖洞、同向自交不互相抵消。差异只在边缘：CoreGraphics 的
 * 抗锯齿同样是逐像素覆盖率，4 行超采样 + 水平解析积分在 8 位量化下
 * 与它相差不超过一两个量化级。
 *
 * 非抗锯齿时取每像素中心一票（与上游 setShouldAntialias(false) 的硬边一致）。
 */
std::vector<uint8_t> path_rasterize_contours(const PathContours& contours, int32_t width,
                                             int32_t height, bool antialias) {
  const size_t pixel_count =
      static_cast<size_t>(width < 0 ? 0 : width) * static_cast<size_t>(height < 0 ? 0 : height);
  std::vector<uint8_t> out(pixel_count, 0);
  if (width <= 0 || height <= 0) return out;

  std::vector<Edge> edges;
  for (const PathContour& contour : contours) add_contour_edges(contour, &edges);
  if (edges.empty()) return out;

  /* 按「起始扫描行」分桶：每行只处理已经开始的边。套索/钢笔可能有几万个
   * 顶点，逐行全量扫边会把一次栅格化变成 O(行数 × 边数)。 */
  std::vector<std::vector<Edge>> buckets(static_cast<size_t>(height));
  for (const Edge& edge : edges) {
    double start = std::floor(edge.y0);
    if (start < 0.0) start = 0.0;
    if (start >= static_cast<double>(height)) continue;
    buckets[static_cast<size_t>(start)].push_back(edge);
  }

  const int32_t samples = antialias ? 4 : 1;
  std::vector<Edge> active;
  std::vector<std::pair<double, int>> crossings;
  std::vector<uint32_t> accumulator(static_cast<size_t>(width), 0u);

  for (int32_t y = 0; y < height; ++y) {
    const std::vector<Edge>& starting = buckets[static_cast<size_t>(y)];
    active.insert(active.end(), starting.begin(), starting.end());
    /* 结束行 <= y 的边对下面所有采样点都不再相交。 */
    size_t keep = 0;
    for (size_t i = 0; i < active.size(); ++i) {
      if (active[i].y1 > static_cast<double>(y)) active[keep++] = active[i];
    }
    active.resize(keep);
    if (active.empty()) {
      /* 没有活跃边：本行及以下若也没有起始边，后面的行同样为空，但桶里
       * 可能还有更低的边，不能直接退出。清零后继续即可。 */
      continue;
    }

    std::fill(accumulator.begin(), accumulator.end(), 0u);
    for (int32_t s = 0; s < samples; ++s) {
      const double sample_y =
          static_cast<double>(y) + (static_cast<double>(s) + 0.5) / static_cast<double>(samples);
      crossings.clear();
      for (const Edge& edge : active) {
        if (sample_y < edge.y0 || sample_y >= edge.y1) continue;
        const double t = (sample_y - edge.y0) / (edge.y1 - edge.y0);
        crossings.emplace_back(edge.x0 + t * (edge.x1 - edge.x0), edge.dir);
      }
      if (crossings.empty()) continue;
      std::sort(crossings.begin(), crossings.end(),
                [](const std::pair<double, int>& lhs, const std::pair<double, int>& rhs) {
                  return lhs.first < rhs.first;
                });
      int winding = 0;
      double span_start = 0.0;
      for (const std::pair<double, int>& crossing : crossings) {
        const int previous = winding;
        winding += crossing.second;
        if (previous == 0 && winding != 0) {
          span_start = crossing.first;
        } else if (previous != 0 && winding == 0) {
          add_horizontal_span(span_start, crossing.first, &accumulator, width);
        }
      }
    }

    uint8_t* dest = out.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int32_t x = 0; x < width; ++x) {
      /* 样本票数 -> 覆盖率：先取整到 255，再按是否抗锯齿二值化。
       * 非抗锯齿只有一个样本，票数就是 0 或 255。 */
      uint32_t value =
          (accumulator[static_cast<size_t>(x)] + static_cast<uint32_t>(samples / 2)) /
          static_cast<uint32_t>(samples);
      if (!antialias) value = value >= 128u ? 255u : 0u;
      dest[x] = static_cast<uint8_t>(value > 255u ? 255u : value);
    }
  }
  return out;
}

PathBounds path_contour_bounds(const PathContours& contours) {
  PathBounds bounds;
  for (const PathContour& contour : contours) {
    for (size_t i = 0; i + 1 < contour.size(); i += 2) {
      const double x = contour[i];
      const double y = contour[i + 1];
      if (!bounds.any) {
        bounds.x0 = bounds.x1 = x;
        bounds.y0 = bounds.y1 = y;
        bounds.any = true;
      } else {
        bounds.x0 = std::min(bounds.x0, x);
        bounds.x1 = std::max(bounds.x1, x);
        bounds.y0 = std::min(bounds.y0, y);
        bounds.y1 = std::max(bounds.y1, y);
      }
    }
  }
  return bounds;
}

}  // namespace compositor

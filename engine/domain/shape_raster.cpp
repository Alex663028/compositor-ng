// AI生成
#include "domain/shape_raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace compositor {

namespace {

/* 描边时每段折线最多切多长再逐块取包围盒：45° 长边整段取包围盒会把
 * 斜线变成「对角线整幅矩形」的逐像素距离计算；切块后每块只扫自己的细长盒，
 * 且共线子段的胶囊并集与整段胶囊**逐像素相等**（不会出现接缝）。 */
constexpr double kStrokeChunk = 32.0;

/* 路径扁平化后的单条轮廓顶点上限（调用方还有总量上限）。 */
constexpr size_t kMaxContourPoints = 1u << 16;

void translate_contours(const PathContours& source, double dx, double dy, PathContours* dest) {
  dest->clear();
  dest->reserve(source.size());
  for (const PathContour& contour : source) {
    PathContour moved;
    moved.resize(contour.size());
    for (size_t i = 0; i + 1 < contour.size(); i += 2) {
      moved[i] = static_cast<float>(static_cast<double>(contour[i]) + dx);
      moved[i + 1] = static_cast<float>(static_cast<double>(contour[i + 1]) + dy);
    }
    dest->push_back(std::move(moved));
  }
}

/* 几何 -> 图层局部坐标下的闭合/开放轮廓。返回 false 表示顶点超限。 */
bool build_contours(const ShapeGeometry& geometry, int32_t origin_x, int32_t origin_y,
                    PathContours* contours) {
  const double ox = static_cast<double>(origin_x);
  const double oy = static_cast<double>(origin_y);
  const double x0 = geometry.x0 - ox;
  const double y0 = geometry.y0 - oy;
  const double x1 = geometry.x1 - ox;
  const double y1 = geometry.y1 - oy;
  size_t total = 0;
  auto over_limit = [&]() { return total > kMaxShapePoints; };

  switch (geometry.kind) {
    case kShapeRasterRect:
      if (geometry.corner_radius > 0.0) {
        path_add_rounded_rect_contour(x0, y0, x1, y1, geometry.corner_radius, contours);
      } else {
        path_add_rect_contour(x0, y0, x1, y1, contours);
      }
      break;
    case kShapeRasterEllipse:
      path_add_ellipse_contour(x0, y0, x1, y1, contours);
      break;
    case kShapeRasterLine:
      contours->push_back(PathContour{static_cast<float>(x0), static_cast<float>(y0),
                                      static_cast<float>(x1), static_cast<float>(y1)});
      break;
    case kShapeRasterPath: {
      if (geometry.segments == nullptr || geometry.segment_count <= 0) return false;
      for (int32_t i = 0; i < geometry.segment_count; ++i) {
        const ShapeCubic& segment = geometry.segments[i];
        double start_x = 0.0;
        double start_y = 0.0;
        if (segment.new_subpath || contours->empty()) {
          contours->emplace_back();
          start_x = segment.start[0] - ox;
          start_y = segment.start[1] - oy;
          contours->back().push_back(static_cast<float>(start_x));
          contours->back().push_back(static_cast<float>(start_y));
        } else {
          const PathContour& previous = contours->back();
          start_x = previous[previous.size() - 2];
          start_y = previous[previous.size() - 1];
        }
        path_flatten_cubic(start_x, start_y, segment.control1[0] - ox, segment.control1[1] - oy,
                           segment.control2[0] - ox, segment.control2[1] - oy,
                           segment.end[0] - ox, segment.end[1] - oy, 0, kMaxContourPoints,
                           &contours->back());
        if (contours->back().size() / 2 > kMaxContourPoints) return false;
      }
      break;
    }
    default:
      return false;
  }
  for (const PathContour& contour : *contours) total += contour.size() / 2;
  return !over_limit();
}

/* 圆头胶囊描边：所有轮廓段各画一条「到线段距离 <= 半宽」的覆盖率带，取最大值。
 * 圆头端点与圆角连接都来自距离并集，不需要另写 join/cap 逻辑。 */
std::vector<uint8_t> rasterize_stroke(const PathContours& contours, bool closed,
                                      double half_width, bool antialias, int32_t window_x,
                                      int32_t window_y, int32_t width, int32_t height) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  if (half_width <= 0.0 || width <= 0 || height <= 0) return out;
  const double pad = half_width + 1.0;

  auto paint = [&](double ax, double ay, double bx, double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double length_sq = dx * dx + dy * dy;
    if (!(length_sq > 0.0)) return;
    const double length = std::sqrt(length_sq);
    const int pieces = static_cast<int>(std::ceil(length / kStrokeChunk));
    const int steps = pieces > 1 ? pieces : 1;
    for (int step = 0; step < steps; ++step) {
      const double t0 = static_cast<double>(step) / static_cast<double>(steps);
      const double t1 = static_cast<double>(step + 1) / static_cast<double>(steps);
      const double sx = ax + dx * t0;
      const double sy = ay + dy * t0;
      const double ex = ax + dx * t1;
      const double ey = ay + dy * t1;

      const double min_x = std::min(sx, ex) - pad;
      const double max_x = std::max(sx, ex) + pad;
      const double min_y = std::min(sy, ey) - pad;
      const double max_y = std::max(sy, ey) + pad;
      int32_t x0 = static_cast<int32_t>(std::floor(min_x)) - window_x;
      int32_t x1 = static_cast<int32_t>(std::ceil(max_x)) - window_x;
      int32_t y0 = static_cast<int32_t>(std::floor(min_y)) - window_y;
      int32_t y1 = static_cast<int32_t>(std::ceil(max_y)) - window_y;
      x0 = std::max<int32_t>(x0, 0);
      y0 = std::max<int32_t>(y0, 0);
      x1 = std::min<int32_t>(x1, width - 1);
      y1 = std::min<int32_t>(y1, height - 1);
      if (x1 < x0 || y1 < y0) continue;

      const double segment_x = ex - sx;
      const double segment_y = ey - sy;
      const double segment_length_sq = segment_x * segment_x + segment_y * segment_y;
      for (int32_t y = y0; y <= y1; ++y) {
        const double py = static_cast<double>(window_y + y) + 0.5;
        uint8_t* row = out.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int32_t x = x0; x <= x1; ++x) {
          const double px = static_cast<double>(window_x + x) + 0.5;
          double t = 0.0;
          if (segment_length_sq > 0.0) {
            t = ((px - sx) * segment_x + (py - sy) * segment_y) / segment_length_sq;
            t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
          }
          const double cx = sx + segment_x * t;
          const double cy = sy + segment_y * t;
          const double dist = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
          double coverage = 0.0;
          if (antialias) {
            /* 覆盖率 = 半宽 + 半像素 - 距离，夹到 0..1：边缘必然占一个像素宽。 */
            coverage = half_width + 0.5 - dist;
          } else {
            coverage = dist <= half_width ? 1.0 : 0.0;
          }
          if (!(coverage > 0.0)) continue;
          if (coverage > 1.0) coverage = 1.0;
          const long value = std::lround(coverage * 255.0);
          const uint8_t byte = static_cast<uint8_t>(value > 255 ? 255 : value);
          uint8_t& pixel = row[static_cast<size_t>(x)];
          if (byte > pixel) pixel = byte;
        }
      }
    }
  };

  for (const PathContour& contour : contours) {
    const size_t count = contour.size() / 2;
    if (count < 2) continue;
    const size_t segment_count = closed ? count : count - 1;
    for (size_t i = 0; i < segment_count; ++i) {
      const size_t j = (i + 1) % count;
      paint(contour[2 * i], contour[2 * i + 1], contour[2 * j], contour[2 * j + 1]);
    }
  }
  return out;
}

}  // namespace

ShapeRasterReport shape_rasterize(const ShapeGeometry& geometry, int32_t origin_x,
                                  int32_t origin_y, int32_t layer_width, int32_t layer_height,
                                  bool fill_enabled, bool stroke_enabled, double stroke_width,
                                  bool antialias, ShapeCoverage* out) {
  ShapeRasterReport report;
  if (out != nullptr) *out = ShapeCoverage{};
  if (layer_width <= 0 || layer_height <= 0) {
    report.status = ShapeRasterStatus::kBadGeometry;
    report.message = "layer has no pixels";
    return report;
  }
  if (!fill_enabled && !stroke_enabled) {
    report.status = ShapeRasterStatus::kBadGeometry;
    report.message = "shape has neither fill nor stroke";
    return report;
  }

  PathContours contours;
  if (!build_contours(geometry, origin_x, origin_y, &contours) || contours.empty()) {
    report.status = ShapeRasterStatus::kTooComplex;
    report.message = "shape path exceeds the flattening budget";
    return report;
  }
  const PathBounds bounds = path_contour_bounds(contours);
  if (!bounds.any) {
    report.status = ShapeRasterStatus::kBadGeometry;
    report.message = "shape path has no vertices";
    return report;
  }

  const double half_width = stroke_enabled ? stroke_width * 0.5 : 0.0;
  /* 描边外扩半宽 + 1 像素抗锯齿边；填充外扩 0（覆盖率在栅格化时按窗口夹取）。 */
  const double expand = stroke_enabled ? half_width + 1.0 : 0.0;
  const double min_x = bounds.x0 - expand;
  const double max_x = bounds.x1 + expand;
  const double min_y = bounds.y0 - expand;
  const double max_y = bounds.y1 + expand;

  int32_t window_x = static_cast<int32_t>(std::floor(min_x));
  int32_t window_y = static_cast<int32_t>(std::floor(min_y));
  int32_t window_right = static_cast<int32_t>(std::ceil(max_x));
  int32_t window_bottom = static_cast<int32_t>(std::ceil(max_y));
  window_x = std::max<int32_t>(window_x, 0);
  window_y = std::max<int32_t>(window_y, 0);
  window_right = std::min<int32_t>(window_right, layer_width);
  window_bottom = std::min<int32_t>(window_bottom, layer_height);
  if (window_right <= window_x || window_bottom <= window_y) {
    report.status = ShapeRasterStatus::kEmpty;
    report.message = "shape does not intersect the layer";
    return report;
  }

  ShapeCoverage result;
  result.x = window_x;
  result.y = window_y;
  result.w = window_right - window_x;
  result.h = window_bottom - window_y;

  if (fill_enabled && geometry.kind != kShapeRasterLine) {
    PathContours windowed;
    translate_contours(contours, -static_cast<double>(window_x), -static_cast<double>(window_y),
                       &windowed);
    result.fill = path_rasterize_contours(windowed, result.w, result.h, antialias);
  }
  if (stroke_enabled) {
    const bool closed = geometry.kind == kShapeRasterPath
                            ? geometry.closed
                            : (geometry.kind != kShapeRasterLine);
    result.stroke = rasterize_stroke(contours, closed, half_width, antialias, window_x, window_y,
                                     result.w, result.h);
  }
  if (out != nullptr) *out = std::move(result);
  return report;
}

}  // namespace compositor

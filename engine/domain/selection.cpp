// AI生成
#include "domain/selection.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace compositor {

namespace {

constexpr double kPi = 3.14159265358979323846;

/* 控制点二阶差分的 L1 范数小于这个值就用直线代替：0.1 像素的弦高误差
 * 折算到 255 级覆盖率不足 0.3 级，低于 8 位量化台阶。 */
constexpr double kFlattenTolerance = 0.1;
constexpr int kFlattenMaxDepth = 20;

/* 「无特征」距离场的哨兵：足够大，加到 (q-p)² 上不溢出 float，
 * 又远大于任何真实画布尺寸（30000² = 9e8）。 */
constexpr float kDistanceInfinity = 1e20f;

/* ------------------------------------------------------------------ 轮廓 */

/* 一条闭合轮廓。坐标是文档像素（左上原点），与 ABI 一致。 */
using Contour = std::vector<float>;
using ContourList = std::vector<Contour>;

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

void add_contour_edges(const Contour& contour, std::vector<Edge>* edges) {
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
std::vector<uint8_t> rasterize_contours(const ContourList& contours, int32_t width,
                                        int32_t height, bool antialias) {
  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  std::vector<uint8_t> out(pixel_count, 0);

  std::vector<Edge> edges;
  for (const Contour& contour : contours) add_contour_edges(contour, &edges);
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

/* ------------------------------------------------------------ 形状 -> 轮廓 */

void add_rect_contour(double x0, double y0, double x1, double y1, ContourList* contours) {
  const double left = std::min(x0, x1);
  const double right = std::max(x0, x1);
  const double top = std::min(y0, y1);
  const double bottom = std::max(y0, y1);
  contours->push_back(Contour{static_cast<float>(left), static_cast<float>(top),
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
void add_ellipse_contour(double x0, double y0, double x1, double y1, ContourList* contours) {
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

  Contour contour;
  contour.reserve(static_cast<size_t>(segments) * 2);
  for (int i = 0; i < segments; ++i) {
    const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(segments);
    contour.push_back(static_cast<float>(cx + rx * std::cos(angle)));
    contour.push_back(static_cast<float>(cy + ry * std::sin(angle)));
  }
  contours->push_back(std::move(contour));
}

/* 自适应细分（de Casteljau 中点分裂）。控制点二阶差分足够小时，
 * 曲线与控制多边形都塌到直线段上，直接收尾。 */
void flatten_cubic(double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                   double y3, int depth, Contour* contour) {
  const double d1x = std::fabs(x0 - 2.0 * x1 + x2);
  const double d1y = std::fabs(y0 - 2.0 * y1 + y2);
  const double d2x = std::fabs(x1 - 2.0 * x2 + x3);
  const double d2y = std::fabs(y1 - 2.0 * y2 + y3);
  if (depth >= kFlattenMaxDepth || (d1x + d1y <= kFlattenTolerance && d2x + d2y <= kFlattenTolerance)) {
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
  flatten_cubic(x0, y0, x01, y01, x012, y012, xm, ym, depth + 1, contour);
  flatten_cubic(xm, ym, x123, y123, x23, y23, x3, y3, depth + 1, contour);
}

/* ------------------------------------------------------------------ 包围盒 */

struct Bounds {
  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 0.0;
  double y1 = 0.0;
  bool any = false;
};

Bounds contour_bounds(const ContourList& contours) {
  Bounds bounds;
  for (const Contour& contour : contours) {
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

/* 基础蒙版里非零像素的闭区间包围盒。 */
bool nonzero_bounds(const MaskBuffer& mask, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
  const int32_t width = mask.width;
  const int32_t height = mask.height;
  if (width <= 0 || height <= 0) return false;
  if (mask.pixel_data().size() != static_cast<size_t>(width) * static_cast<size_t>(height)) {
    return false;
  }
  int32_t min_x = width;
  int32_t min_y = height;
  int32_t max_x = -1;
  int32_t max_y = -1;
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* row = mask.row(y);
    for (int32_t x = 0; x < width; ++x) {
      if (row[x] == 0) continue;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
      if (y < min_y) min_y = y;
      if (y > max_y) max_y = y;
    }
  }
  if (max_x < 0) return false;
  *x0 = min_x;
  *y0 = min_y;
  *x1 = max_x;
  *y1 = max_y;
  return true;
}

/* ------------------------------------------------------------ 选区落盘 */

bool masks_equal(const std::vector<uint8_t>& lhs, const std::vector<uint8_t>& rhs) {
  return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

void store_base(SelectionState* selection, int32_t width, int32_t height,
                std::vector<uint8_t>&& values) {
  MaskBuffer next;
  next.width = width;
  next.height = height;
  next.pixels = std::make_shared<std::vector<uint8_t>>(std::move(values));
  selection->base = std::move(next);
}

/* ------------------------------------------------------------ 形状叠加 */

/*
 * 所有形状操作落到这里的唯一入口。contours 已经裁过非法输入、也算好了
 * 包围盒；这里只处理叠加语义与状态落盘。
 *
 * 上游 applySelection 的等效口径：
 *   - replace：base = 新形状；add：base = max(base, 形状)；
 *     subtract：base = base * (1 - 形状)（8 位乘法，等效于把形状当 alpha 擦掉）；
 *   - **任何**成功的形状操作都把 feather 归 0；
 *   - 退化形状（包围盒宽或高 <= 0）在 replace 下等价于 deselect，
 *     其它模式下忽略；
 *   - 结果与旧 base 完全一致且 feather 本来就是 0 时 changed = false，
 *     宿主据此不产生撤销步（上游 setSelection 的等值短路）。
 */
bool apply_contours(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                    const ContourList& contours, const Bounds& bounds, int32_t combine,
                    bool antialias, bool* changed) {
  const bool degenerate = !(bounds.x1 > bounds.x0) || !(bounds.y1 > bounds.y0);

  if (degenerate) {
    /* 上游 finishLasso 的 guard：退化形状在 replace 下 = 取消选择。 */
    if (combine != kSelectionReplace) return true;
    const bool was_active = selection->active;
    selection->active = false;
    selection->feather = 0.0f;
    selection->base = MaskBuffer{};
    if (changed != nullptr) *changed = was_active;
    return true;
  }

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  const bool same_grid = selection->active && selection->base.width == canvas_width &&
                         selection->base.height == canvas_height &&
                         selection->base.pixel_data().size() == pixel_count;
  const std::vector<uint8_t>* current = same_grid ? &selection->base.pixel_data() : nullptr;

  /* subtract 没有基础选区时什么都不做（上游 guard let current = selection）。
   * 蒙版尺寸与画布不符属内部不变量被破坏（正常路径不会出现），同样拒绝。 */
  if (combine == kSelectionSubtract && !same_grid) return true;

  std::vector<uint8_t> shape = rasterize_contours(contours, canvas_width, canvas_height, antialias);

  /* 从无选区（或尺寸不符）到有选区本身就是变化；feather 非零时也会被重置。 */
  bool next_changed = !same_grid || selection->feather != 0.0f;

  std::vector<uint8_t> next;
  switch (combine) {
    case kSelectionAdd: {
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t base_value = current != nullptr ? (*current)[i] : 0;
        next[i] = std::max(base_value, shape[i]);
      }
      break;
    }
    case kSelectionSubtract: {
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const double keep = 255.0 - static_cast<double>(shape[i]);
        next[i] = static_cast<uint8_t>(
            std::lround(static_cast<double>((*current)[i]) * keep / 255.0));
      }
      break;
    }
    default: {
      /* replace：尺寸不符时等价于从空蒙版开始（防御性，正常状态不会出现）。 */
      next = std::move(shape);
      break;
    }
  }

  if (!next_changed) {
    next_changed = !masks_equal(next, *current);
  }
  if (!next_changed) return true; /* 等值短路：不改状态、不进历史 */

  selection->active = true;
  selection->feather = 0.0f;
  store_base(selection, canvas_width, canvas_height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

/* -------------------------------------------------------- 距离场（扩/缩） */

/*
 * 一维精确平方距离变换（Felzenszwalb & Huttenlocher, "Distance Transforms of
 * Sampled Functions"）。
 *
 * 输入 f[q] 是 q 处「到最近特征点的平方距离」的初值（0 = 特征，kDistanceInfinity
 * = 无特征），输出 d[q] = min_p ((q - p)² + f[p])。下包络用单调栈维护：
 * 每个抛物线最多进栈一次、出栈一次，整体 O(n)。
 *
 * v/z 是调用方的复用缓冲（长度 >= n 与 n + 1），避免逐行分配。
 */
void edt_1d(const float* f, int n, int* v, double* z, float* d) {
  int k = 0;
  v[0] = 0;
  z[0] = -std::numeric_limits<double>::infinity();
  z[1] = std::numeric_limits<double>::infinity();
  for (int q = 1; q < n; ++q) {
    double intersection = 0.0;
    for (;;) {
      const double fq = static_cast<double>(f[q]) + static_cast<double>(q) * static_cast<double>(q);
      const double fv =
          static_cast<double>(f[v[k]]) + static_cast<double>(v[k]) * static_cast<double>(v[k]);
      intersection = (fq - fv) / (2.0 * static_cast<double>(q - v[k]));
      if (intersection <= z[k]) {
        --k; /* 第 k 个抛物线被完全遮住 */
      } else {
        break;
      }
    }
    ++k;
    v[k] = q;
    z[k] = intersection;
    z[k + 1] = std::numeric_limits<double>::infinity();
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < static_cast<double>(q)) ++k;
    const double diff = static_cast<double>(q - v[k]);
    d[q] = static_cast<float>(diff * diff + static_cast<double>(f[v[k]]));
  }
}

/*
 * 二维精确欧氏距离场（先列后行）。seeds_are_inside 为真时 inside 里的 1
 * 是特征点，否则 0 是特征点——只用一个布尔量，省掉一整张种子缓冲。
 */
void edt_2d(const std::vector<uint8_t>& inside, int32_t width, int32_t height,
            bool seeds_are_inside, std::vector<float>* out) {
  std::vector<float> grid(static_cast<size_t>(width) * static_cast<size_t>(height));
  for (size_t i = 0; i < grid.size(); ++i) {
    const bool seed = seeds_are_inside ? (inside[i] != 0) : (inside[i] == 0);
    grid[i] = seed ? 0.0f : kDistanceInfinity;
  }

  const int max_side = std::max(width, height);
  std::vector<int> v(static_cast<size_t>(max_side));
  std::vector<double> z(static_cast<size_t>(max_side) + 1);
  std::vector<float> line(static_cast<size_t>(max_side));
  std::vector<float> result(static_cast<size_t>(max_side));

  /* 垂直：逐列搬进 line（edt_1d 的输入输出不能别名），变换后写回网格。 */
  for (int32_t x = 0; x < width; ++x) {
    for (int32_t y = 0; y < height; ++y) {
      line[static_cast<size_t>(y)] = grid[static_cast<size_t>(y) * width + x];
    }
    edt_1d(line.data(), height, v.data(), z.data(), result.data());
    for (int32_t y = 0; y < height; ++y) {
      grid[static_cast<size_t>(y) * width + x] = result[static_cast<size_t>(y)];
    }
  }

  /* 水平：同理，行先复制到 line，结果直接写回网格行。 */
  for (int32_t y = 0; y < height; ++y) {
    float* row = grid.data() + static_cast<size_t>(y) * width;
    std::copy(row, row + width, line.begin());
    edt_1d(line.data(), width, v.data(), z.data(), row);
  }
  *out = std::move(grid);
}

/*
 * 扩展 / 收缩（对齐上游 copy(strokingWithWidth:) 的圆角外扩内缩）。
 *
 * 用符号距离判断每个像素的新覆盖率，而不是真去「描边再栅格化」：
 *   - 覆盖率 255 的像素：它到最近「未选中」像素的距离 d_out；
 *   - 覆盖率 0 的像素：它到最近「选中」像素的距离 d_in；
 *   - 覆盖率 128 的像素：直接按覆盖率取深度。
 * 覆盖率 c 的像素等价于「边界落在像素中心 + (c/255 - 0.5)」处，于是
 *   新覆盖率 = clamp(0.5 + (深度 ± amount))，像素坐标下 depth 以「像素中心 = 0」度量。
 *
 * 窗口 = 包围盒外扩 |amount| + 2，窗口外一律按「未选中」处理（比 amount
 * 更远的地方外扩/内缩都不可能改变结果）。窗口贴着画布边时，外面那一圈
 * 虚拟像素表示画布外——Contract 因此会连画布边缘一起内缩（上游同款），
 * Expand 则被画布裁掉。
 */
bool resize_base(const SelectionState& selection, int32_t delta, std::vector<uint8_t>* out) {
  const int32_t width = selection.base.width;
  const int32_t height = selection.base.height;
  if (width <= 0 || height <= 0) return false;
  if (selection.base.pixel_data().size() !=
      static_cast<size_t>(width) * static_cast<size_t>(height)) {
    return false;
  }

  int32_t bx0 = 0;
  int32_t by0 = 0;
  int32_t bx1 = 0;
  int32_t by1 = 0;
  if (!nonzero_bounds(selection.base, &bx0, &by0, &bx1, &by1)) return false;

  const int32_t pad = std::abs(delta) + 2;
  const int32_t wx0 = std::max(0, bx0 - pad);
  const int32_t wy0 = std::max(0, by0 - pad);
  const int32_t wx1 = std::min(width, bx1 + 1 + pad);  /* 半开 */
  const int32_t wy1 = std::min(height, by1 + 1 + pad);
  const int32_t ww = wx1 - wx0;
  const int32_t wh = wy1 - wy0;

  out->assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

  /* 网格 = 窗口 + 1 像素外框；外框永远是「未选中」。 */
  const int32_t gw = ww + 2;
  const int32_t gh = wh + 2;
  const size_t grid_size = static_cast<size_t>(gw) * static_cast<size_t>(gh);
  std::vector<uint8_t> inside(grid_size, 0);
  for (int32_t y = 0; y < wh; ++y) {
    const uint8_t* source = selection.base.row(wy0 + y);
    uint8_t* target = inside.data() + static_cast<size_t>(y + 1) * gw + 1;
    for (int32_t x = 0; x < ww; ++x) {
      target[x] = source[wx0 + x] >= 128 ? 1 : 0;
    }
  }

  std::vector<float> field;
  std::vector<uint8_t> next_window(static_cast<size_t>(ww) * static_cast<size_t>(wh), 0);
  const float shift = static_cast<float>(delta);
  const MaskBuffer& base = selection.base;

  /* 一趟：覆盖率 255 的像素用「到最近未选中像素」的距离（扩展与收缩都要）。 */
  edt_2d(inside, gw, gh, /*seeds_are_inside=*/false, &field);
  for (int32_t y = 0; y < wh; ++y) {
    const uint8_t* source = base.row(wy0 + y);
    for (int32_t x = 0; x < ww; ++x) {
      if (source[wx0 + x] < 255) continue;
      const size_t grid_index = static_cast<size_t>(y + 1) * gw + (x + 1);
      const float depth = std::sqrt(field[grid_index]) - 0.5f;
      const float coverage = std::min(1.0f, std::max(0.0f, 0.5f + depth + shift));
      next_window[static_cast<size_t>(y) * ww + x] =
          static_cast<uint8_t>(std::lround(coverage * 255.0f));
    }
  }

  /* 二趟：覆盖率 0 的像素只有**扩展**会改变（收缩恒为 0，跳过省一半时间）。 */
  if (delta > 0) {
    edt_2d(inside, gw, gh, /*seeds_are_inside=*/true, &field);
    for (int32_t y = 0; y < wh; ++y) {
      const uint8_t* source = base.row(wy0 + y);
      for (int32_t x = 0; x < ww; ++x) {
        if (source[wx0 + x] != 0) continue;
        const size_t grid_index = static_cast<size_t>(y + 1) * gw + (x + 1);
        const float depth = 0.5f - std::sqrt(field[grid_index]);
        const float coverage = std::min(1.0f, std::max(0.0f, 0.5f + depth + shift));
        next_window[static_cast<size_t>(y) * ww + x] =
            static_cast<uint8_t>(std::lround(coverage * 255.0f));
      }
    }
  }

  /* 三趟：中间覆盖率直接按「覆盖率 - 0.5」平移深度（亚像素边缘）。 */
  for (int32_t y = 0; y < wh; ++y) {
    const uint8_t* source = base.row(wy0 + y);
    for (int32_t x = 0; x < ww; ++x) {
      const uint8_t value = source[wx0 + x];
      if (value == 0 || value == 255) continue;
      const float depth = static_cast<float>(value) / 255.0f - 0.5f;
      const float coverage = std::min(1.0f, std::max(0.0f, 0.5f + depth + shift));
      next_window[static_cast<size_t>(y) * ww + x] =
          static_cast<uint8_t>(std::lround(coverage * 255.0f));
    }
  }

  /* 写回整幅蒙版（窗口外保持 0）。 */
  for (int32_t y = 0; y < wh; ++y) {
    uint8_t* target = out->data() + static_cast<size_t>(wy0 + y) * width + wx0;
    const uint8_t* source = next_window.data() + static_cast<size_t>(y) * ww;
    std::copy(source, source + ww, target);
  }
  return true;
}

/* ------------------------------------------------------------------ 羽化 */

/* 一维盒式模糊（边缘钳制 = 上游 clampedToExtent）：滑动窗口 O(1) / 像素。 */
void box_blur_horizontal(const uint16_t* src, uint16_t* dst, int32_t width, int32_t height,
                         int32_t radius) {
  const int32_t window = 2 * radius + 1;
  for (int32_t y = 0; y < height; ++y) {
    const uint16_t* line = src + static_cast<size_t>(y) * width;
    uint16_t* out = dst + static_cast<size_t>(y) * width;
    int32_t sum = 0;
    for (int32_t i = -radius; i <= radius; ++i) {
      const int32_t x = i < 0 ? 0 : (i >= width ? width - 1 : i);
      sum += line[x];
    }
    for (int32_t x = 0; x < width; ++x) {
      out[x] = static_cast<uint16_t>((sum + window / 2) / window);
      int32_t add = x + radius + 1;
      int32_t sub = x - radius;
      if (add > width - 1) add = width - 1;
      if (sub < 0) sub = 0;
      sum += line[add] - line[sub];
    }
  }
}

void box_blur_vertical(const uint16_t* src, uint16_t* dst, int32_t width, int32_t height,
                       int32_t radius) {
  const int32_t window = 2 * radius + 1;
  for (int32_t x = 0; x < width; ++x) {
    int32_t sum = 0;
    for (int32_t i = -radius; i <= radius; ++i) {
      const int32_t y = i < 0 ? 0 : (i >= height ? height - 1 : i);
      sum += src[static_cast<size_t>(y) * width + x];
    }
    for (int32_t y = 0; y < height; ++y) {
      dst[static_cast<size_t>(y) * width + x] =
          static_cast<uint16_t>((sum + window / 2) / window);
      int32_t add = y + radius + 1;
      int32_t sub = y - radius;
      if (add > height - 1) add = height - 1;
      if (sub < 0) sub = 0;
      sum += src[static_cast<size_t>(add) * width + x] -
             src[static_cast<size_t>(sub) * width + x];
    }
  }
}

/*
 * 羽化 = 高斯模糊（sigma = feather / 2，上游 applyingGaussianBlur(sigma: feather/2)）。
 *
 * 两条路径：
 *   - sigma <= 2.5：精确可分离高斯（半径 ceil(3σ)，模板和归一化）。半径 <= 8，
 *     逐像素 17 次乘加，常见羽化半径（1..5 像素）都走这条；
 *   - sigma > 2.5：三趟盒式模糊近似高斯（Kovesi 的方差匹配：r(r+1)(2r+1)/6 组
 *     三趟等价 r(r+1) 的平方和；取 r = round((sqrt(4σ²+1) - 1) / 2)）。大半径
 *     时滑动窗口是 O(1) / 像素，不会退化成几千次乘加。
 * 中间结果用 uint16 存，单趟舍入误差 <= 0.5 级。
 *
 * 边缘按 clampedToExtent 钳制：全选羽化不会在画布边缘变淡（上游同款）。
 */
void feather_blur(const std::vector<uint8_t>& source, int32_t width, int32_t height, float sigma,
                  std::vector<uint8_t>* out) {
  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (sigma <= 0.05f) {
    *out = source;
    return;
  }

  std::vector<uint16_t> buffer_a(pixel_count, 0);
  std::vector<uint16_t> buffer_b(pixel_count, 0);
  out->assign(pixel_count, 0);

  if (sigma <= 2.5f) {
    const int32_t radius = std::max(1, static_cast<int32_t>(std::ceil(3.0 * sigma)));
    std::vector<float> kernel(static_cast<size_t>(2 * radius + 1), 0.0f);
    float total = 0.0f;
    for (int32_t i = -radius; i <= radius; ++i) {
      const float value =
          std::exp(-static_cast<float>(i) * static_cast<float>(i) /
                   (2.0f * sigma * sigma));
      kernel[static_cast<size_t>(i + radius)] = value;
      total += value;
    }
    for (float& value : kernel) value /= total;

    /* 水平：source(u8) -> buffer_a(u16)。 */
    for (int32_t y = 0; y < height; ++y) {
      const uint8_t* line = source.data() + static_cast<size_t>(y) * width;
      uint16_t* target = buffer_a.data() + static_cast<size_t>(y) * width;
      for (int32_t x = 0; x < width; ++x) {
        float sum = 0.0f;
        for (int32_t k = -radius; k <= radius; ++k) {
          int32_t sx = x + k;
          if (sx < 0) sx = 0;
          if (sx > width - 1) sx = width - 1;
          sum += kernel[static_cast<size_t>(k + radius)] * static_cast<float>(line[sx]);
        }
        target[x] = static_cast<uint16_t>(std::lround(sum));
      }
    }
    /* 垂直：buffer_a(u16) -> out(u8)。 */
    for (int32_t y = 0; y < height; ++y) {
      uint8_t* target = out->data() + static_cast<size_t>(y) * width;
      for (int32_t x = 0; x < width; ++x) {
        float sum = 0.0f;
        for (int32_t k = -radius; k <= radius; ++k) {
          int32_t sy = y + k;
          if (sy < 0) sy = 0;
          if (sy > height - 1) sy = height - 1;
          sum += kernel[static_cast<size_t>(k + radius)] *
                 static_cast<float>(buffer_a[static_cast<size_t>(sy) * width + x]);
        }
        const long value = std::lround(sum);
        target[x] = static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
      }
    }
    return;
  }

  int32_t radius = static_cast<int32_t>(
      std::lround((std::sqrt(1.0 + 4.0 * static_cast<double>(sigma) * sigma) - 1.0) * 0.5));
  if (radius < 1) radius = 1;
  /* 半径不能超过边长，否则窗口钳制后的有效方差与公式不再匹配；
   * 这种极端 sigma（>= 边长）结果本来就是全画布均值。 */
  if (radius > std::max(width, height)) radius = std::max(width, height);

  for (size_t i = 0; i < pixel_count; ++i) buffer_a[i] = source[i];
  for (int pass = 0; pass < 3; ++pass) {
    box_blur_horizontal(buffer_a.data(), buffer_b.data(), width, height, radius);
    box_blur_vertical(buffer_b.data(), buffer_a.data(), width, height, radius);
  }
  for (size_t i = 0; i < pixel_count; ++i) {
    (*out)[i] = static_cast<uint8_t>(buffer_a[i] > 255 ? 255 : buffer_a[i]);
  }
}

}  // namespace

/* ------------------------------------------------------------------ 公开 API */

bool selection_apply_shape(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                           int32_t shape, const float* points, int32_t point_count,
                           int32_t combine, bool antialias, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (canvas_width <= 0 || canvas_height <= 0) return false;
  if (shape < kSelectionRect || shape > kSelectionPolygon) return false;
  if (combine < kSelectionReplace || combine > kSelectionSubtract) return false;
  if (points == nullptr || point_count <= 0) return false;
  if (point_count > kMaxSelectionPoints) return false;
  if (shape == kSelectionPolygon) {
    if (point_count < 3) return false;
  } else if (point_count != 2) {
    return false;
  }
  for (int32_t i = 0; i < point_count * 2; ++i) {
    if (!std::isfinite(points[i])) return false;
  }

  ContourList contours;
  if (shape == kSelectionRect) {
    add_rect_contour(points[0], points[1], points[2], points[3], &contours);
  } else if (shape == kSelectionEllipse) {
    add_ellipse_contour(points[0], points[1], points[2], points[3], &contours);
  } else {
    contours.emplace_back(points, points + static_cast<size_t>(point_count) * 2);
  }

  return apply_contours(selection, canvas_width, canvas_height, contours,
                        contour_bounds(contours), combine, antialias, changed);
}

bool selection_apply_path(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          const SelectionCubic* segments, int32_t segment_count, int32_t combine,
                          bool antialias, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (canvas_width <= 0 || canvas_height <= 0) return false;
  if (combine < kSelectionReplace || combine > kSelectionSubtract) return false;
  if (segments == nullptr || segment_count <= 0) return false;
  if (segment_count > kMaxSelectionSegments) return false;
  for (int32_t i = 0; i < segment_count; ++i) {
    const SelectionCubic& segment = segments[i];
    const float values[8] = {segment.start[0],    segment.start[1],    segment.control1[0],
                             segment.control1[1], segment.control2[0], segment.control2[1],
                             segment.end[0],      segment.end[1]};
    for (float value : values) {
      if (!std::isfinite(value)) return false;
    }
  }

  ContourList contours;
  size_t finished_points = 0;
  for (int32_t i = 0; i < segment_count; ++i) {
    const SelectionCubic& segment = segments[i];
    double start_x = 0.0;
    double start_y = 0.0;
    if (segment.new_subpath || contours.empty()) {
      if (!contours.empty()) finished_points += contours.back().size() / 2;
      if (finished_points > static_cast<size_t>(kMaxSelectionPoints)) return false;
      contours.emplace_back();
      start_x = segment.start[0];
      start_y = segment.start[1];
      contours.back().push_back(static_cast<float>(start_x));
      contours.back().push_back(static_cast<float>(start_y));
    } else {
      const Contour& previous = contours.back();
      start_x = previous[previous.size() - 2];
      start_y = previous[previous.size() - 1];
    }
    flatten_cubic(start_x, start_y, segment.control1[0], segment.control1[1], segment.control2[0],
                  segment.control2[1], segment.end[0], segment.end[1], 0, &contours.back());
    if (contours.back().size() / 2 + finished_points >
        static_cast<size_t>(kMaxSelectionPoints)) {
      return false;
    }
  }

  return apply_contours(selection, canvas_width, canvas_height, contours,
                        contour_bounds(contours), combine, antialias, changed);
}

bool selection_select_all(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (canvas_width <= 0 || canvas_height <= 0) return false;

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  if (selection->active && selection->feather == 0.0f &&
      selection->base.width == canvas_width && selection->base.height == canvas_height &&
      selection->base.pixel_data().size() == pixel_count &&
      std::all_of(selection->base.pixel_data().begin(), selection->base.pixel_data().end(),
                  [](uint8_t value) { return value == 255; })) {
    return true; /* 已经是全选：等值短路 */
  }

  store_base(selection, canvas_width, canvas_height, std::vector<uint8_t>(pixel_count, 255));
  selection->active = true;
  selection->feather = 0.0f;
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_deselect(SelectionState* selection, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (!selection->active) return true; /* 上游 setSelection(nil) 前的 guard */
  selection->active = false;
  selection->feather = 0.0f;
  selection->base = MaskBuffer{};
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_invert(SelectionState* selection, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (!selection->active) return true;

  const std::vector<uint8_t>& base = selection->base.pixel_data();
  const size_t pixel_count = base.size();
  if (pixel_count == 0) return true;

  bool all_selected = true;
  for (uint8_t value : base) {
    if (value != 255) {
      all_selected = false;
      break;
    }
  }
  if (all_selected) {
    /* 反选全选 = 无选区（上游 inverse.isEmpty ? nil : inverse）。 */
    selection->active = false;
    selection->feather = 0.0f;
    selection->base = MaskBuffer{};
    if (changed != nullptr) *changed = true;
    return true;
  }

  std::vector<uint8_t> next(pixel_count, 0);
  bool different = false;
  for (size_t i = 0; i < pixel_count; ++i) {
    next[i] = static_cast<uint8_t>(255 - base[i]);
    if (next[i] != base[i]) different = true;
  }
  if (!different) return true;

  /* 反选保留 feather（上游只翻转 path，不动羽化半径）。 */
  store_base(selection, selection->base.width, selection->base.height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_modify(SelectionState* selection, int32_t op, int32_t amount, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (op < kSelectionFeather || op > kSelectionContract) return false;
  const int32_t limit = op == kSelectionFeather ? kMaxSelectionFeather : kMaxSelectionGrow;
  if (amount < 1 || amount > limit) return false;
  /* 没有选区或显式空选区：上游 canModifySelection == false，一律无操作。 */
  if (!selection->active || selection_is_empty(*selection)) return true;

  if (op == kSelectionFeather) {
    const float current = selection->feather;
    const float requested = static_cast<float>(amount);
    float next = std::sqrt(current * current + requested * requested);
    if (next > static_cast<float>(kMaxSelectionFeather)) {
      next = static_cast<float>(kMaxSelectionFeather);
    }
    if (next == current) return true;
    selection->feather = next;
    if (changed != nullptr) *changed = true;
    return true;
  }

  const int32_t delta = op == kSelectionExpand ? amount : -amount;
  std::vector<uint8_t> next;
  if (!resize_base(*selection, delta, &next)) return false;
  if (masks_equal(next, selection->base.pixel_data())) return true; /* 等值短路 */
  store_base(selection, selection->base.width, selection->base.height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

/* ------------------------------------------------------------ 载入 / 平移 */

/*
 * 图层像素（alpha）或蒙版灰度在文档坐标 (doc_x, doc_y) 处的值。
 * 映射与 compositing/resample.cpp 的逆变换逐字一致：归一化 -> 逆旋转 ->
 * 翻转 -> 图层像素坐标；采样用最近邻（选区是硬边蒙版，插值只会把边缘
 * 抹成半选，语义上不该发生）。图层外返回 0。
 */
uint8_t sample_layer_source(const Layer& layer, bool use_mask, double doc_x, double doc_y) {
  const int32_t sw = use_mask ? layer.mask.width : layer.pixels.width;
  const int32_t sh = use_mask ? layer.mask.height : layer.pixels.height;
  if (sw <= 0 || sh <= 0) return 0;

  int32_t ix = 0;
  int32_t iy = 0;
  if (!layer.document_to_pixel(sw, sh, doc_x, doc_y, &ix, &iy)) return 0;

  if (use_mask) {
    return layer.mask.pixel_data()[static_cast<size_t>(iy) * static_cast<size_t>(sw) +
                                   static_cast<size_t>(ix)];
  }

  const std::vector<uint8_t>& px = layer.pixels.pixel_data();
  const size_t offset = static_cast<size_t>(iy) * layer.pixels.stride() +
                        static_cast<size_t>(ix) * static_cast<size_t>(bytes_per_pixel(layer.pixels.format));
  if (offset + static_cast<size_t>(bytes_per_pixel(layer.pixels.format)) > px.size()) return 0;

  switch (layer.pixels.format) {
    case COMP_PIXEL_RGBA8:
      return px[offset + 3];
    case COMP_PIXEL_RGBA16: {
      uint16_t value = 0;
      std::memcpy(&value, px.data() + offset + 6, sizeof(value));
      return static_cast<uint8_t>((static_cast<uint32_t>(value) * 255u + 32767u) / 65535u);
    }
    case COMP_PIXEL_RGBA16F: {
      uint16_t bits = 0;
      std::memcpy(&bits, px.data() + offset + 6, sizeof(bits));
      /* half -> 0..1：符号位/指数/尾数手工展开。NaN 记 0，超白截到 1。 */
      const uint32_t sign = bits >> 15;
      const uint32_t exponent = (bits >> 10) & 0x1fu;
      const uint32_t mantissa = bits & 0x3ffu;
      double value = 0.0;
      if (sign == 0) {
        if (exponent == 0) {
          value = static_cast<double>(mantissa) * std::ldexp(1.0, -24);
        } else if (exponent != 31) {
          value = (1.0 + static_cast<double>(mantissa) / 1024.0) *
                  std::ldexp(1.0, static_cast<int>(exponent) - 15);
        } else if (mantissa == 0) {
          value = 65504.0; /* +inf：按超白处理 */
        }
      }
      if (!(value > 0.0)) return 0;
      if (value >= 1.0) return 255;
      return static_cast<uint8_t>(std::lround(value * 255.0));
    }
    default:
      return 0;
  }
}

/* 与 apply_contours 同款的叠加语义，但输入已经是文档分辨率的 coverage。 */
bool apply_coverage_mask(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                         std::vector<uint8_t>&& coverage, int32_t combine, bool* changed) {
  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  const bool same_grid = selection->active && selection->base.width == canvas_width &&
                         selection->base.height == canvas_height &&
                         selection->base.pixel_data().size() == pixel_count;
  const std::vector<uint8_t>* current = same_grid ? &selection->base.pixel_data() : nullptr;

  /* subtract 没有基础选区时什么都不做（与形状叠加一致）。 */
  if (combine == kSelectionSubtract && !same_grid) return true;

  std::vector<uint8_t> next;
  switch (combine) {
    case kSelectionAdd: {
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t base_value = current != nullptr ? (*current)[i] : 0;
        next[i] = std::max(base_value, coverage[i]);
      }
      break;
    }
    case kSelectionSubtract: {
      next.assign(pixel_count, 0);
      for (size_t i = 0; i < pixel_count; ++i) {
        const double keep = 255.0 - static_cast<double>(coverage[i]);
        next[i] = static_cast<uint8_t>(
            std::lround(static_cast<double>((*current)[i]) * keep / 255.0));
      }
      break;
    }
    default:
      next = std::move(coverage);
      break;
  }

  bool next_changed = !same_grid || selection->feather != 0.0f;
  if (!next_changed) next_changed = !masks_equal(next, *current);
  if (!next_changed) return true;

  selection->active = true;
  selection->feather = 0.0f;
  store_base(selection, canvas_width, canvas_height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_load_layer(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                          const Layer& layer, int32_t source, int32_t combine, int32_t threshold,
                          bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (canvas_width <= 0 || canvas_height <= 0) return false;
  if (source != kSelectionLoadPixels && source != kSelectionLoadMask) return false;
  if (combine < kSelectionReplace || combine > kSelectionSubtract) return false;
  if (threshold < 0 || threshold > 255) return false;

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  std::vector<uint8_t> coverage(pixel_count, 0);

  const bool use_mask = source == kSelectionLoadMask;
  const int32_t sw = use_mask ? layer.mask.width : layer.pixels.width;
  const int32_t sh = use_mask ? layer.mask.height : layer.pixels.height;
  const comp_rect bounds = layer.effective_bounds();

  if (sw > 0 && sh > 0 && bounds.w > 0 && bounds.h > 0) {
    /* 只在图层覆盖的文档矩形里采样：矩形外一定为 0，跳过可以省掉整幅遍历。 */
    const int32_t left = std::max(0, bounds.x);
    const int32_t top = std::max(0, bounds.y);
    const int64_t right = std::min<int64_t>(canvas_width, static_cast<int64_t>(bounds.x) + bounds.w);
    const int64_t bottom = std::min<int64_t>(canvas_height, static_cast<int64_t>(bounds.y) + bounds.h);
    for (int32_t y = top; y < bottom; ++y) {
      uint8_t* row = coverage.data() + static_cast<size_t>(y) * static_cast<size_t>(canvas_width);
      for (int32_t x = left; x < right; ++x) {
        const uint8_t value =
            sample_layer_source(layer, use_mask, static_cast<double>(x), static_cast<double>(y));
        row[x] = value > threshold ? value : 0;
      }
    }
  }

  return apply_coverage_mask(selection, canvas_width, canvas_height, std::move(coverage), combine,
                             changed);
}

bool selection_translate(SelectionState* selection, int32_t dx, int32_t dy, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (!selection->active) return true;

  const int32_t width = selection->base.width;
  const int32_t height = selection->base.height;
  if (width <= 0 || height <= 0) return true;
  const std::vector<uint8_t>& base = selection->base.pixel_data();
  if (base.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return true;

  std::vector<uint8_t> next(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  if (dx == 0 && dy == 0) {
    next = base;
  } else {
    /* 源区间 [left, right) x [top, bottom)。dx > 0 时源列从 0 开始、目标列从 dx
     * 开始；dx < 0 时源列从 -dx 开始、目标列从 0 开始。因此目标列偏移是
     * left + dx（= max(dx, 0)），不是 dx——负数直接转 size_t 会把指针甩到缓冲区前。
     * 平移量超出画布（right <= left 或 bottom <= top）时没有重叠区，直接跳过；
     * 负数长度交给 memcpy 就是堆损坏。 */
    const int64_t left = std::max<int64_t>(0, -static_cast<int64_t>(dx));
    const int64_t right = std::min<int64_t>(width, static_cast<int64_t>(width) - dx);
    const int64_t top = std::max<int64_t>(0, -static_cast<int64_t>(dy));
    const int64_t bottom = std::min<int64_t>(height, static_cast<int64_t>(height) - dy);
    const int64_t dst_x = left + dx;
    if (right > left && bottom > top) {
      for (int64_t y = top; y < bottom; ++y) {
        const uint8_t* src = base.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        uint8_t* dst = next.data() + static_cast<size_t>(y + dy) * static_cast<size_t>(width) +
                       static_cast<size_t>(dst_x);
        std::memcpy(dst, src + left, static_cast<size_t>(right - left));
      }
    }
  }

  if (masks_equal(next, base)) return true; /* 等值短路：空选区 / 原地平移 */
  store_base(selection, width, height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

void selection_effective_coverage(const SelectionState& selection, std::vector<uint8_t>* out) {
  if (out == nullptr) return;
  out->clear();
  if (!selection.active) return;
  const int32_t width = selection.base.width;
  const int32_t height = selection.base.height;
  if (width <= 0 || height <= 0) return;
  const std::vector<uint8_t>& base = selection.base.pixel_data();
  if (base.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return;
  if (selection.feather <= 0.0f) {
    *out = base;
    return;
  }
  feather_blur(base, width, height, selection.feather * 0.5f, out);
}

comp_rect selection_bounds(const SelectionState& selection) {
  comp_rect result{0, 0, 0, 0};
  if (!selection.active) return result;

  int32_t x0 = 0;
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
  if (!nonzero_bounds(selection.base, &x0, &y0, &x1, &y1)) return result;

  /* 羽化的可见衰减区：上游 coverageBounds = bbox.insetBy(-ceil(feather * 2))。
   * 那多留的 1 像素是给 clip() 的裁切安全边，不是可见范围，这里不外扩。 */
  const int32_t pad = static_cast<int32_t>(std::ceil(static_cast<double>(selection.feather) * 2.0));
  x0 -= pad;
  y0 -= pad;
  x1 += pad;
  y1 += pad;
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > selection.base.width - 1) x1 = selection.base.width - 1;
  if (y1 > selection.base.height - 1) y1 = selection.base.height - 1;
  if (x1 < x0 || y1 < y0) return result;

  result.x = x0;
  result.y = y0;
  result.w = x1 - x0 + 1;
  result.h = y1 - y0 + 1;
  return result;
}

bool selection_is_empty(const SelectionState& selection) {
  if (!selection.active) return true;
  const std::vector<uint8_t>& base = selection.base.pixel_data();
  for (uint8_t value : base) {
    if (value != 0) return false;
  }
  return true;
}

}  // namespace compositor

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
#include <string>
#include <utility>
#include <vector>

#include "compositing/compositor.h"
#include "domain/document.h"
#include "domain/path_raster.h"
#include "imaging/WandPixels.h"

namespace compositor {

namespace {

/* 「无特征」距离场的哨兵：足够大，加到 (q-p)² 上不溢出 float，
 * 又远大于任何真实画布尺寸（30000² = 9e8）。 */
constexpr float kDistanceInfinity = 1e20f;

/* 轮廓/扁平化/扫描填充的唯一实现在 domain/path_raster.*（形状工具 S10 共用）。 */
using Contour = PathContour;
using ContourList = PathContours;
using Bounds = PathBounds;

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

  std::vector<uint8_t> shape = path_rasterize_contours(contours, canvas_width, canvas_height, antialias);

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

/* ------------------------------------------------------------------ 边界 */

/*
 * 边界（Border）：沿选区轮廓保留一条**总宽 amount 像素**的环带。
 *
 * 用「外扩 ceil(amount/2)」减「内缩 floor(amount/2)」表达，两趟都复用
 * resize_base 的精确距离场与抗锯齿覆盖率，所以环带内外沿仍是软的，不会
 * 突然变成硬边。环带总宽恒为 amount，奇偶半径都对称（奇数时外侧多 1 px）：
 *   amount = 1 -> 外扩 1 px − 原选区，只有外侧 1 px；
 *   amount = 2 -> 外扩 1 px − 内缩 1 px，外 1 + 内 1；
 *   amount = 3 -> 外扩 2 px − 内缩 1 px，外 2 + 内 1。
 * 参数口径取「环带总宽」而不是「单侧宽度」：与 PS 的 Select > Modify > Border
 * 一致（Border 21 得到约 21 px 宽的环带，而不是两侧各 21 px）。
 * 覆盖率相减用 8 位乘法（a * (255 - b) / 255），不是布尔与非——半选的边缘
 * 相减后仍然是半选，这正是 PS 里「羽化后的选区做边界」看到的样子。
 */
bool border_base(const SelectionState& selection, int32_t amount, std::vector<uint8_t>* out) {
  if (amount < 1) return false;
  std::vector<uint8_t> outer;
  if (!resize_base(selection, (amount + 1) / 2, &outer)) return false;

  const int32_t shrink = amount / 2;
  std::vector<uint8_t> inner;
  if (shrink > 0) {
    if (!resize_base(selection, -shrink, &inner)) return false;
  } else {
    inner = selection.base.pixel_data();
  }
  if (inner.size() != outer.size()) return false;

  out->resize(outer.size());
  for (size_t i = 0; i < outer.size(); ++i) {
    const int32_t band = static_cast<int32_t>(outer[i]) * (255 - static_cast<int32_t>(inner[i]));
    (*out)[i] = static_cast<uint8_t>((band + 127) / 255);
  }
  return true;
}

/* ------------------------------------------------------------------ 平滑 */

/*
 * 二值形态学的一步：用欧氏距离场的符号判断像素是否保留，半径按像素定义。
 * 与 resize_base 同口径——覆盖率满的像素到最近反色像素的距离减去 0.5 才是
 * 「到边界」的距离，所以半径 1 恰好削掉/长出一圈整像素。
 */
std::vector<uint8_t> erode_binary(const std::vector<uint8_t>& inside, int32_t width,
                                  int32_t height, int32_t radius) {
  std::vector<float> field;
  edt_2d(inside, width, height, /*seeds_are_inside=*/false, &field);
  std::vector<uint8_t> out(inside.size(), 0);
  const float limit = static_cast<float>(radius);
  for (size_t i = 0; i < inside.size(); ++i) {
    if (inside[i] == 0) continue;
    if (std::sqrt(field[i]) - 0.5f >= limit) out[i] = 1;
  }
  return out;
}

std::vector<uint8_t> dilate_binary(const std::vector<uint8_t>& inside, int32_t width,
                                   int32_t height, int32_t radius) {
  std::vector<float> field;
  edt_2d(inside, width, height, /*seeds_are_inside=*/true, &field);
  std::vector<uint8_t> out(inside.size(), 0);
  const float limit = static_cast<float>(radius);
  for (size_t i = 0; i < inside.size(); ++i) {
    if (inside[i] != 0) {
      out[i] = 1;
      continue;
    }
    if (std::sqrt(field[i]) - 0.5f <= limit) out[i] = 1;
  }
  return out;
}

/*
 * 平滑（Smooth）：先开后闭的形态学，圆化凹陷与凸起但不动直边。
 *
 *   open  = dilate(erode(mask, N), N)  —— 削掉比 2N 窄的凸起/毛刺；
 *   close = erode(dilate(open, N), N)  —— 补上比 2N 窄的缺口。
 *
 * 直边在开+闭下逐像素还原（先削一圈再长回一圈），所以「本来就规整的矩形
 * 选区做平滑」是恒等变换、不产生撤销步；只有拐角/锯齿才会被磨圆。这与 PS
 * 「Smooth 把边界磨成圆弧」的观感一致，也是为什么它不能用模糊+阈值实现
 * （那会把直边也糊掉）。
 *
 * 只在 bounds 外扩 N+2 的窗口里算；窗口外本来就是 0，形态学也不会把选区
 * 长出这个范围。窗口贴着画布边时，外层补一圈 0 表示画布外——与 resize_base
 * 的 Contract 同口径（选区贴边时平滑会向内收，而不是把画布外当选中）。
 * 输出是 0/255 的二值蒙版：平滑不生产新的半选边缘（半选由 feather 负责）。
 */
bool smooth_base(const SelectionState& selection, int32_t radius, std::vector<uint8_t>* out) {
  const int32_t width = selection.base.width;
  const int32_t height = selection.base.height;
  if (width <= 0 || height <= 0) return false;
  const std::vector<uint8_t>& base = selection.base.pixel_data();
  if (base.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return false;

  int32_t bx0 = 0;
  int32_t by0 = 0;
  int32_t bx1 = 0;
  int32_t by1 = 0;
  if (!nonzero_bounds(selection.base, &bx0, &by0, &bx1, &by1)) return false;

  const int32_t pad = radius + 2;
  const int32_t wx0 = std::max(0, bx0 - pad);
  const int32_t wy0 = std::max(0, by0 - pad);
  const int32_t wx1 = std::min(width, bx1 + 1 + pad); /* 半开 */
  const int32_t wy1 = std::min(height, by1 + 1 + pad);
  const int32_t ww = wx1 - wx0;
  const int32_t wh = wy1 - wy0;

  const int32_t gw = ww + 2;
  const int32_t gh = wh + 2;
  std::vector<uint8_t> grid(static_cast<size_t>(gw) * static_cast<size_t>(gh), 0);
  for (int32_t y = 0; y < wh; ++y) {
    const uint8_t* source = selection.base.row(wy0 + y);
    uint8_t* target = grid.data() + static_cast<size_t>(y + 1) * gw + 1;
    for (int32_t x = 0; x < ww; ++x) {
      target[x] = source[wx0 + x] >= 128 ? 1 : 0;
    }
  }

  const std::vector<uint8_t> opened = dilate_binary(
      erode_binary(grid, gw, gh, radius), gw, gh, radius);
  const std::vector<uint8_t> closed = erode_binary(
      dilate_binary(opened, gw, gh, radius), gw, gh, radius);

  out->assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < wh; ++y) {
    uint8_t* target = out->data() + static_cast<size_t>(wy0 + y) * width + wx0;
    const uint8_t* source = closed.data() + static_cast<size_t>(y + 1) * gw + 1;
    for (int32_t x = 0; x < ww; ++x) {
      target[x] = source[x] != 0 ? 255 : 0;
    }
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
    path_add_rect_contour(points[0], points[1], points[2], points[3], &contours);
  } else if (shape == kSelectionEllipse) {
    path_add_ellipse_contour(points[0], points[1], points[2], points[3], &contours);
  } else {
    contours.emplace_back(points, points + static_cast<size_t>(point_count) * 2);
  }

  return apply_contours(selection, canvas_width, canvas_height, contours,
                        path_contour_bounds(contours), combine, antialias, changed);
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
    path_flatten_cubic(start_x, start_y, segment.control1[0], segment.control1[1], segment.control2[0],
                  segment.control2[1], segment.end[0], segment.end[1], 0,
                  static_cast<size_t>(kMaxSelectionPoints), &contours.back());
    if (contours.back().size() / 2 + finished_points >
        static_cast<size_t>(kMaxSelectionPoints)) {
      return false;
    }
  }

  return apply_contours(selection, canvas_width, canvas_height, contours,
                        path_contour_bounds(contours), combine, antialias, changed);
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
  if (op < kSelectionFeather || op > kSelectionSmooth) return false;
  const int32_t limit = op == kSelectionFeather ? kMaxSelectionFeather
                        : op == kSelectionSmooth ? kMaxSelectionSmooth
                                                 : kMaxSelectionGrow;
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

  std::vector<uint8_t> next;
  if (op == kSelectionBorder) {
    if (!border_base(*selection, amount, &next)) return false;
  } else if (op == kSelectionSmooth) {
    if (!smooth_base(*selection, amount, &next)) return false;
  } else {
    const int32_t delta = op == kSelectionExpand ? amount : -amount;
    if (!resize_base(*selection, delta, &next)) return false;
  }
  if (masks_equal(next, selection->base.pixel_data())) return true; /* 等值短路 */
  store_base(selection, selection->base.width, selection->base.height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_set_feather(SelectionState* selection, int32_t feather, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (feather < 0 || feather > kMaxSelectionFeather) return false;
  /* 没有选区或显式空选区：羽化半径没有承载对象，无操作（与 modify 同闸）。 */
  if (!selection->active || selection_is_empty(*selection)) return true;

  const float next = static_cast<float>(feather);
  if (next == selection->feather) return true; /* 等值短路：不产生撤销步 */
  selection->feather = next;
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

/* --------------------------------------------------------------- 魔棒 */

namespace {

/* 取样面统一落到 RGBA8（预乘）：16 位/HDR 走既有的 8 位工作副本。
 * 返回的像素指针可能指向 source 自身，也可能指向 scratch。 */
bool wand_rgba8_view(const ImageBuffer& source, ImageBuffer* scratch,
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

comp_result selection_apply_wand(SelectionState* selection, int32_t canvas_width,
                                 int32_t canvas_height, const Document& document,
                                 comp_layer_id layer, const SelectionWandParams& params,
                                 int32_t combine, bool* changed, std::string* error) {
  if (changed != nullptr) *changed = false;
  const auto fail = [error](comp_result code, const char* text) {
    if (error != nullptr) *error = text;
    return code;
  };

  if (selection == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "selection is null");
  if (canvas_width <= 0 || canvas_height <= 0) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand needs a non-empty canvas");
  }
  if (combine < kSelectionReplace || combine > kSelectionSubtract) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if (params.source != kSelectionWandLayer && params.source != kSelectionWandMerged) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "unknown wand source");
  }
  if (params.tolerance < 0 || params.tolerance > 255) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand tolerance out of 0..255");
  }
  if (params.radius < 0 || params.radius > kMaxSelectionWandRadius) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand radius out of range");
  }
  if (params.contiguous != 0 && params.contiguous != 1) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand contiguous must be 0 or 1");
  }
  if (!std::isfinite(params.seed_x) || !std::isfinite(params.seed_y)) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand seed is not finite");
  }

  /* 指针在文档像素上取整：魔棒是逐像素操作，亚像素落点取所在像素。 */
  const int64_t doc_x = static_cast<int64_t>(std::floor(params.seed_x));
  const int64_t doc_y = static_cast<int64_t>(std::floor(params.seed_y));
  if (doc_x < 0 || doc_y < 0 || doc_x >= canvas_width || doc_y >= canvas_height) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand seed lies outside the canvas");
  }

  ImageBuffer composite;
  ImageBuffer scratch;
  const std::vector<uint8_t>* pixels = nullptr;
  size_t stride = 0;
  int32_t grid_width = 0;
  int32_t grid_height = 0;
  int64_t origin_x = 0;
  int64_t origin_y = 0;

  if (params.source == kSelectionWandMerged) {
    std::string compose_error;
    if (!composite_document(document, &composite, &compose_error)) {
      return fail(COMP_ERR_INTERNAL, "wand could not composite the document");
    }
    if (!wand_rgba8_view(composite, &scratch, &pixels, &stride)) {
      return fail(COMP_ERR_INTERNAL, "wand could not build an 8-bit composite");
    }
    grid_width = canvas_width;
    grid_height = canvas_height;
  } else {
    const Layer* target = document.find(layer);
    if (target == nullptr) return fail(COMP_ERR_NOT_FOUND, "wand source layer was not found");
    if (target->is_group) {
      return fail(COMP_ERR_STATE, "group layers have no pixels to sample");
    }
    if (target->pixels.empty()) {
      return fail(COMP_ERR_STATE, "wand source layer has no pixels");
    }
    if (!wand_rgba8_view(target->pixels, &scratch, &pixels, &stride)) {
      return fail(COMP_ERR_STATE, "wand source layer could not be sampled as 8-bit RGBA");
    }
    /* 泛洪网格 = 图层像素网格，映射回文档必须是**整数平移**：缩放/旋转/翻转
     * 图层先栅格化再选（宿主侧的光栅化是显式动作，不能在这里静默降级）。 */
    const comp_rect bounds = target->effective_bounds();
    if (bounds.w != target->pixels.width || bounds.h != target->pixels.height) {
      return fail(COMP_ERR_STATE, "magic wand needs a layer with a 1:1 transform");
    }
    grid_width = target->pixels.width;
    grid_height = target->pixels.height;
    origin_x = bounds.x;
    origin_y = bounds.y;
  }

  const int64_t grid_x = doc_x - origin_x;
  const int64_t grid_y = doc_y - origin_y;
  if (grid_x < 0 || grid_y < 0 || grid_x >= grid_width || grid_y >= grid_height) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "wand seed lies outside the sampled surface");
  }

  std::vector<uint8_t> mask(static_cast<size_t>(grid_width) * static_cast<size_t>(grid_height), 0);
  const long selected =
      wand_mask(pixels->data(), static_cast<size_t>(grid_width), static_cast<size_t>(grid_height),
                stride, static_cast<size_t>(grid_x), static_cast<size_t>(grid_y),
                static_cast<size_t>(params.radius), params.tolerance, params.contiguous,
                mask.data());
  if (selected < 0) return fail(COMP_ERR_OUT_OF_MEMORY, "wand ran out of memory");
  /* 0 个匹配不改选区：与 COMP_TOOL_SELECT「没选中就不写蒙版」同口径，
   * 否则在空处点一下会静默清掉用户已有的选区。 */
  if (selected == 0) return COMP_OK;

  std::vector<uint8_t> coverage(
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height), 0);
  const int64_t left = std::max<int64_t>(0, origin_x);
  const int64_t top = std::max<int64_t>(0, origin_y);
  const int64_t right = std::min<int64_t>(canvas_width, origin_x + grid_width);
  const int64_t bottom = std::min<int64_t>(canvas_height, origin_y + grid_height);
  for (int64_t y = top; y < bottom; ++y) {
    uint8_t* row = coverage.data() + static_cast<size_t>(y) * static_cast<size_t>(canvas_width);
    const uint8_t* src = mask.data() +
                         static_cast<size_t>(y - origin_y) * static_cast<size_t>(grid_width) +
                         static_cast<size_t>(left - origin_x);
    std::memcpy(row + left, src, static_cast<size_t>(right - left));
  }

  if (!apply_coverage_mask(selection, canvas_width, canvas_height, std::move(coverage), combine,
                           changed)) {
    return fail(COMP_ERR_INTERNAL, "selection coverage could not be updated");
  }
  return COMP_OK;
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

/* ------------------------------------------------------- 手柄重采样（S46） */

/* 双线性取样：画布外的点按 0 处理（基础蒙版在紧包围盒外本来就是 0，
 * 这条规则只影响缩放到包围盒外沿的那一圈目标像素）。 */
double sample_mask_bilinear(const std::vector<uint8_t>& base, int32_t width, int32_t height,
                            double x, double y) {
  const int32_t x0 = static_cast<int32_t>(std::floor(x));
  const int32_t y0 = static_cast<int32_t>(std::floor(y));
  const double fx = x - static_cast<double>(x0);
  const double fy = y - static_cast<double>(y0);
  const auto at = [&base, width, height](int32_t px, int32_t py) -> double {
    if (px < 0 || py < 0 || px >= width || py >= height) return 0.0;
    return static_cast<double>(base[static_cast<size_t>(py) * static_cast<size_t>(width) + px]);
  };
  const double top = at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx;
  const double bottom = at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx;
  return top * (1.0 - fy) + bottom * fy;
}

comp_rect selection_base_bounds(const SelectionState& selection) {
  comp_rect result{0, 0, 0, 0};
  if (!selection.active) return result;
  int32_t x0 = 0;
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
  if (!nonzero_bounds(selection.base, &x0, &y0, &x1, &y1)) return result;
  result.x = x0;
  result.y = y0;
  result.w = x1 - x0 + 1;
  result.h = y1 - y0 + 1;
  return result;
}

bool selection_transform_bounds(SelectionState* selection, comp_rect target, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;
  if (!selection->active) return true;

  const int32_t width = selection->base.width;
  const int32_t height = selection->base.height;
  if (width <= 0 || height <= 0) return false;
  const std::vector<uint8_t>& base = selection->base.pixel_data();
  if (base.size() != static_cast<size_t>(width) * static_cast<size_t>(height)) return false;

  const comp_rect source = selection_base_bounds(*selection);
  std::vector<uint8_t> next(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

  if (source.w > 0 && source.h > 0 && target.w > 0 && target.h > 0) {
    /* 目标矩形按画布裁剪；缩放比仍按**原样**的 target 算，否则贴着画布边界
     * 拖动时内容会被悄悄拉伸。int64 中间量避免 target.x + target.w 溢出。 */
    const int64_t x_begin = std::max<int64_t>(0, target.x);
    const int64_t x_end = std::min<int64_t>(width, static_cast<int64_t>(target.x) + target.w);
    const int64_t y_begin = std::max<int64_t>(0, target.y);
    const int64_t y_end = std::min<int64_t>(height, static_cast<int64_t>(target.y) + target.h);

    const double scale_x = static_cast<double>(source.w) / static_cast<double>(target.w);
    const double scale_y = static_cast<double>(source.h) / static_cast<double>(target.h);

    for (int64_t y = y_begin; y < y_end; ++y) {
      /* 目标像素中心 → 源像素坐标：源像素中心在整数格点上，所以再减 0.5。
       * target == source 时这里恰好得到整数，双线性权重全落在原像素上，
       * 因此恒等变换逐字节不变。 */
      const double sy = (static_cast<double>(y) + 0.5 - static_cast<double>(target.y)) * scale_y +
                        static_cast<double>(source.y) - 0.5;
      uint8_t* row = next.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
      for (int64_t x = x_begin; x < x_end; ++x) {
        const double sx =
            (static_cast<double>(x) + 0.5 - static_cast<double>(target.x)) * scale_x +
            static_cast<double>(source.x) - 0.5;
        const double value = sample_mask_bilinear(base, width, height, sx, sy);
        long rounded = std::lround(value);
        if (rounded < 0) rounded = 0;
        if (rounded > 255) rounded = 255;
        row[static_cast<size_t>(x)] = static_cast<uint8_t>(rounded);
      }
    }
  }

  if (masks_equal(next, base)) return true; /* 恒等 / 空选区 / 收缩到画布外 */
  store_base(selection, width, height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

bool selection_transform_rotated(SelectionState* selection,
                                 const SelectionTransformRect& transform, bool* changed) {
  if (changed != nullptr) *changed = false;
  if (selection == nullptr) return false;

  /* 参数闸：非有限值不进蒙版；否则 NaN 会静默变成「全 0」或未定义行为。 */
  if (!std::isfinite(transform.center_x) || !std::isfinite(transform.center_y) ||
      !std::isfinite(transform.width) || !std::isfinite(transform.height) ||
      !std::isfinite(transform.angle_degrees)) {
    return false;
  }
  if (!selection->active) return true;

  const int32_t width = selection->base.width;
  const int32_t height = selection->base.height;
  if (width <= 0 || height <= 0) return false;
  const std::vector<uint8_t>& base = selection->base.pixel_data();
  const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (base.size() != pixel_count) return false;

  /* 退化尺寸 = 收成空选区：与 selection_transform_bounds 的退化分支同口径。 */
  if (transform.width <= 0.0 || transform.height <= 0.0) {
    const comp_rect empty{0, 0, 0, 0};
    return selection_transform_bounds(selection, empty, changed);
  }

  const comp_rect source = selection_base_bounds(*selection);
  if (source.w <= 0 || source.h <= 0) return true; /* 显式空选区：无操作 */

  double angle = std::fmod(transform.angle_degrees, 360.0);
  if (angle < 0.0) angle += 360.0;

  const double half_w = transform.width * 0.5;
  const double half_h = transform.height * 0.5;
  const double left = transform.center_x - half_w;
  const double top = transform.center_y - half_h;

  if (angle == 0.0) {
    /* 轴对齐：交给既有重采样，保证「同中心同尺寸」是逐字节恒等。 */
    comp_rect target{};
    target.x = static_cast<int32_t>(std::lround(left));
    target.y = static_cast<int32_t>(std::lround(top));
    target.w = static_cast<int32_t>(std::lround(transform.width));
    target.h = static_cast<int32_t>(std::lround(transform.height));
    return selection_transform_bounds(selection, target, changed);
  }

  constexpr double kPi = 3.14159265358979323846;
  const double radians = angle * kPi / 180.0;
  const double cos_a = std::cos(radians);
  const double sin_a = std::sin(radians);
  const double scale_x = static_cast<double>(source.w) / transform.width;
  const double scale_y = static_cast<double>(source.h) / transform.height;

  /* 目标区域 = 旋转后四个角点的轴对齐包围盒。 */
  const double unrotated_x[4] = {left, left + transform.width, left + transform.width, left};
  const double unrotated_y[4] = {top, top, top + transform.height, top + transform.height};
  double min_x = 0.0;
  double max_x = 0.0;
  double min_y = 0.0;
  double max_y = 0.0;
  for (int index = 0; index < 4; ++index) {
    const double dx = unrotated_x[index] - transform.center_x;
    const double dy = unrotated_y[index] - transform.center_y;
    const double rotated_x = transform.center_x + dx * cos_a - dy * sin_a;
    const double rotated_y = transform.center_y + dx * sin_a + dy * cos_a;
    if (index == 0) {
      min_x = max_x = rotated_x;
      min_y = max_y = rotated_y;
    } else {
      min_x = std::min(min_x, rotated_x);
      max_x = std::max(max_x, rotated_x);
      min_y = std::min(min_y, rotated_y);
      max_y = std::max(max_y, rotated_y);
    }
  }

  /* 先夹到画布再转 int64：非有限值（理论上已被上面的闸拦下）按 0 处理。 */
  auto span = [](double low, double high, int64_t limit, int64_t* begin, int64_t* end) {
    double first = std::floor(low);
    double last = std::ceil(high);
    if (!(first > 0.0)) first = 0.0;
    if (!(last < static_cast<double>(limit))) last = static_cast<double>(limit);
    if (!(last > first)) {
      *begin = 0;
      *end = 0;
      return;
    }
    *begin = static_cast<int64_t>(first);
    *end = static_cast<int64_t>(last);
  };
  int64_t x_begin = 0;
  int64_t x_end = 0;
  int64_t y_begin = 0;
  int64_t y_end = 0;
  span(min_x, max_x, width, &x_begin, &x_end);
  span(min_y, max_y, height, &y_begin, &y_end);

  std::vector<uint8_t> next(pixel_count, 0);
  for (int64_t y = y_begin; y < y_end; ++y) {
    const double dy = static_cast<double>(y) + 0.5 - transform.center_y;
    uint8_t* row = next.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
    for (int64_t x = x_begin; x < x_end; ++x) {
      const double dx = static_cast<double>(x) + 0.5 - transform.center_x;
      /* 逆旋转回未旋转矩形坐标系（y 向下、顺时针为正 => 逆变换取 -θ）。 */
      const double qx = transform.center_x + dx * cos_a + dy * sin_a;
      const double qy = transform.center_y - dx * sin_a + dy * cos_a;
      const double sx = (qx - left) * scale_x + static_cast<double>(source.x) - 0.5;
      const double sy = (qy - top) * scale_y + static_cast<double>(source.y) - 0.5;
      long rounded = std::lround(sample_mask_bilinear(base, width, height, sx, sy));
      if (rounded < 0) rounded = 0;
      if (rounded > 255) rounded = 255;
      row[static_cast<size_t>(x)] = static_cast<uint8_t>(rounded);
    }
  }

  if (masks_equal(next, base)) return true; /* 例如 360° 或空选区 */
  store_base(selection, width, height, std::move(next));
  if (changed != nullptr) *changed = true;
  return true;
}

comp_result selection_apply_color_range(SelectionState* selection, int32_t canvas_width,
                                        int32_t canvas_height, const Document& document,
                                        const uint8_t* include, size_t include_count,
                                        const uint8_t* exclude, size_t exclude_count,
                                        int32_t fuzziness, int32_t invert, int32_t combine,
                                        bool* changed, std::string* error) {
  if (changed != nullptr) *changed = false;
  const auto fail = [error](comp_result code, const char* text) {
    if (error != nullptr) *error = text;
    return code;
  };
  if (selection == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "selection is null");
  if (canvas_width <= 0 || canvas_height <= 0) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "color range needs a non-empty canvas");
  }
  if (combine < kSelectionReplace || combine > kSelectionSubtract) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "unknown selection combine");
  }
  if ((include == nullptr && include_count > 0) || (exclude == nullptr && exclude_count > 0)) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "color range color array is null but count is nonzero");
  }
  if (fuzziness < 0 || fuzziness > kMaxColorRangeFuzziness) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "color range fuzziness out of 0..200");
  }

  ImageBuffer composite;
  std::string compose_error;
  if (!composite_document(document, &composite, &compose_error)) {
    return fail(COMP_ERR_INTERNAL, "color range could not composite the document");
  }
  if (composite.width != canvas_width || composite.height != canvas_height) {
    return fail(COMP_ERR_INTERNAL, "color range composite size does not match the canvas");
  }

  /* 16 位 / HDR 合成转 8 位视图（与魔棒同一家族）。 */
  const std::vector<uint8_t>* pixels = nullptr;
  size_t stride = 0;
  ImageBuffer scratch;
  if (composite.format == COMP_PIXEL_RGBA8) {
    pixels = &composite.pixel_data();
    stride = composite.stride();
  } else if (hdr_to_rgba8(composite, &scratch) || deep_to_rgba8(composite, &scratch)) {
    pixels = &scratch.pixel_data();
    stride = scratch.stride();
  } else {
    return fail(COMP_ERR_UNSUPPORTED, "color range could not build an 8-bit composite view");
  }

  const size_t pixel_count = static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  std::vector<uint8_t> coverage(pixel_count, 0);

  /* 像素 p 匹配 ⇔ ∃ c ∈ include: 三通道差都 <= fuzziness，且 ∄ e ∈ exclude 满足。
   * include 为空时无匹配（invert 则等效全选）。 */
  const auto matches_any = [fuzziness](const uint8_t* p, const uint8_t* colors,
                                      size_t count) -> bool {
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* c = colors + i * 3;
      if (std::abs(static_cast<int32_t>(p[0]) - static_cast<int32_t>(c[0])) <= fuzziness &&
          std::abs(static_cast<int32_t>(p[1]) - static_cast<int32_t>(c[1])) <= fuzziness &&
          std::abs(static_cast<int32_t>(p[2]) - static_cast<int32_t>(c[2])) <= fuzziness) {
        return true;
      }
    }
    return false;
  };

  size_t selected = 0;
  for (int32_t y = 0; y < canvas_height; ++y) {
    const uint8_t* row = pixels->data() + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < canvas_width; ++x) {
      const uint8_t* p = row + static_cast<size_t>(x) * 4;
      bool hit = matches_any(p, include, include_count);
      if (hit && exclude != nullptr && exclude_count > 0 &&
          matches_any(p, exclude, exclude_count)) {
        hit = false; /* 被排除色覆盖（上游 include/exclude 双表语义） */
      }
      bool keep = invert != 0 ? !hit : hit;
      if (keep) {
        coverage[static_cast<size_t>(y) * static_cast<size_t>(canvas_width) + x] = 255;
        ++selected;
      }
    }
  }

  /* 0 个匹配不改选区：与魔棒「点了个空处不留下空蒙版」同口径。 */
  if (selected == 0) return COMP_OK;
  if (changed != nullptr) *changed = true;
  return apply_coverage_mask(selection, canvas_width, canvas_height, std::move(coverage), combine,
                             changed)
             ? COMP_OK
             : fail(COMP_ERR_INTERNAL, "color range could not merge the coverage");
}

/* ------------------------------------------------- 扩大 / 选取相似（P0-5） */

namespace {

/* 颜色空间占位表：每通道 0..255 一格，256³ = 16 MiB（只活在一次「选取相似」
 * 调用里）。用它做精确的 Chebyshev 膨胀，而不是把颜色量化到 64 级近似——
 * 选区的边界会直接体现在像素上，量化误差会被用户看见。 */
constexpr int32_t kColorVolumeSide = 256;
constexpr size_t kColorVolumeCells =
    static_cast<size_t>(kColorVolumeSide) * kColorVolumeSide * kColorVolumeSide;

/* 一维二值窗口最大值（半径 radius，越界截断），原地写回。
 * line 是带步长的一条线：第 i 个元素在 base[i * stride]。 */
void dilate_line(uint8_t* base, int32_t stride, int32_t length, int32_t radius,
                 std::vector<uint8_t>* scratch) {
  if (radius <= 0 || length <= 0) return;
  scratch->resize(static_cast<size_t>(length));
  uint8_t* source = scratch->data();
  for (int32_t i = 0; i < length; ++i) source[i] = base[static_cast<size_t>(i) * stride];
  int32_t count = 0;
  const int32_t first_end = std::min(radius, length - 1);
  for (int32_t i = 0; i <= first_end; ++i) count += source[i] != 0 ? 1 : 0;
  for (int32_t i = 0; i < length; ++i) {
    base[static_cast<size_t>(i) * stride] = count > 0 ? 1 : 0;
    const int32_t add = i + 1 + radius;
    const int32_t remove = i - radius;
    if (add < length) count += source[add] != 0 ? 1 : 0;
    if (remove >= 0) count -= source[remove] != 0 ? 1 : 0;
  }
}

/* 256³ 占位表的三维 Chebyshev 膨胀 = 三次可分离的一维窗口最大值。
 * 膨胀后 v[c] = 1 ⇔ 存在原来置位的颜色 s，使 |c - s| 每通道 <= radius。 */
void dilate_color_volume(std::vector<uint8_t>* volume, int32_t radius) {
  if (radius <= 0) return;
  const int32_t side = kColorVolumeSide;
  uint8_t* data = volume->data();
  std::vector<uint8_t> scratch;
  /* x 轴（步长 1） */
  for (int32_t z = 0; z < side; ++z) {
    for (int32_t y = 0; y < side; ++y) {
      dilate_line(data + (static_cast<size_t>(z) * side + y) * side, 1, side, radius, &scratch);
    }
  }
  /* y 轴（步长 side） */
  for (int32_t z = 0; z < side; ++z) {
    for (int32_t x = 0; x < side; ++x) {
      dilate_line(data + static_cast<size_t>(z) * side * side + x, side, side, radius, &scratch);
    }
  }
  /* z 轴（步长 side²） */
  for (int32_t y = 0; y < side; ++y) {
    for (int32_t x = 0; x < side; ++x) {
      dilate_line(data + static_cast<size_t>(y) * side + x, side * side, side, radius, &scratch);
    }
  }
}

inline size_t color_key(const uint8_t* rgba) {
  return (static_cast<size_t>(rgba[0]) << 16) | (static_cast<size_t>(rgba[1]) << 8) |
         static_cast<size_t>(rgba[2]);
}

}  // namespace

/* 两条命令共用的前半段：参数闸 + 基础蒙版尺寸校验 + 8 位合成视图。 */
namespace {

/* 返回 COMP_OK 且 *out_pixels / *out_stride 可用；否则返回错误码。
 * *out_ready = false 表示「没有选区或蒙版为空」，调用方直接返回 COMP_OK。 */
comp_result grow_similar_prepare(SelectionState* selection, int32_t canvas_width,
                                 int32_t canvas_height, const Document& document,
                                 int32_t tolerance, const char* command,
                                 const std::vector<uint8_t>** out_pixels, size_t* out_stride,
                                 ImageBuffer* composite, ImageBuffer* scratch, bool* out_ready,
                                 std::string* error) {
  const auto fail = [error](comp_result code, const std::string& text) {
    if (error != nullptr) *error = text;
    return code;
  };
  *out_ready = false;
  if (selection == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "selection is null");
  if (canvas_width <= 0 || canvas_height <= 0) {
    return fail(COMP_ERR_INVALID_ARGUMENT, std::string(command) + " needs a non-empty canvas");
  }
  if (tolerance < 0 || tolerance > 255) {
    return fail(COMP_ERR_INVALID_ARGUMENT, std::string(command) + " tolerance out of 0..255");
  }
  if (!selection->active) return COMP_OK;

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  if (selection->base.width != canvas_width || selection->base.height != canvas_height ||
      selection->base.pixel_data().size() != pixel_count) {
    return fail(COMP_ERR_STATE, "selection mask size does not match the canvas");
  }

  std::string compose_error;
  if (!composite_document(document, composite, &compose_error)) {
    return fail(COMP_ERR_INTERNAL, std::string(command) + " could not composite the document");
  }
  if (composite->width != canvas_width || composite->height != canvas_height) {
    return fail(COMP_ERR_INTERNAL, std::string(command) + " composite size does not match canvas");
  }
  if (!wand_rgba8_view(*composite, scratch, out_pixels, out_stride)) {
    return fail(COMP_ERR_INTERNAL, std::string(command) + " could not build an 8-bit composite");
  }
  *out_ready = true;
  return COMP_OK;
}

}  // namespace

comp_result selection_grow(SelectionState* selection, int32_t canvas_width, int32_t canvas_height,
                           const Document& document, int32_t tolerance, bool* changed,
                           std::string* error) {
  if (changed != nullptr) *changed = false;
  const auto fail = [error](comp_result code, const char* text) {
    if (error != nullptr) *error = text;
    return code;
  };

  ImageBuffer composite;
  ImageBuffer scratch;
  const std::vector<uint8_t>* pixels = nullptr;
  size_t stride = 0;
  bool ready = false;
  const comp_result code =
      grow_similar_prepare(selection, canvas_width, canvas_height, document, tolerance, "grow",
                           &pixels, &stride, &composite, &scratch, &ready, error);
  if (code != COMP_OK) return code;
  if (!ready) return COMP_OK;

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  const std::vector<uint8_t>& base = selection->base.pixel_data();
  /* 原有蒙版逐字节保留（抗锯齿边缘不被抹平），新并入的像素写 255。 */
  std::vector<uint8_t> next(base);
  /* 只按种子数预留：满幅大文档上 reserve(pixel_count) 会先吃掉「画布像素数
   * × 4 字节」（8k × 8k 就是 256 MiB），而选区通常只占很小一块；BFS 追加的
   * 元素走 vector 摊还增长，峰值仍然只与真实扩展量成正比。 */
  size_t seeds = 0;
  for (size_t i = 0; i < pixel_count; ++i) {
    if (base[i] != 0) ++seeds;
  }
  /* 基础蒙版为空：没有种子，不改选区。 */
  if (seeds == 0) return COMP_OK;

  std::vector<int32_t> queue;
  queue.reserve(seeds);
  for (size_t i = 0; i < pixel_count; ++i) {
    if (base[i] != 0) queue.push_back(static_cast<int32_t>(i));
  }

  size_t head = 0;
  size_t added = 0;
  const int32_t dx[4] = {-1, 1, 0, 0};
  const int32_t dy[4] = {0, 0, -1, 1};
  while (head < queue.size()) {
    const int32_t index = queue[head++];
    const int32_t x = index % canvas_width;
    const int32_t y = index / canvas_width;
    const uint8_t* center =
        pixels->data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4;
    for (int k = 0; k < 4; ++k) {
      const int32_t nx = x + dx[k];
      const int32_t ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= canvas_width || ny >= canvas_height) continue;
      const int32_t neighbor = ny * canvas_width + nx;
      if (next[static_cast<size_t>(neighbor)] != 0) continue;
      const uint8_t* cand = pixels->data() + static_cast<size_t>(ny) * stride +
                            static_cast<size_t>(nx) * 4;
      int32_t max_diff = 0;
      for (int c = 0; c < 4; ++c) {
        const int32_t diff = std::abs(static_cast<int32_t>(cand[c]) -
                                      static_cast<int32_t>(center[c]));
        if (diff > max_diff) max_diff = diff;
      }
      if (max_diff > tolerance) continue;
      next[static_cast<size_t>(neighbor)] = 255;
      ++added;
      queue.push_back(neighbor);
    }
  }

  if (added == 0) return COMP_OK;
  if (!apply_coverage_mask(selection, canvas_width, canvas_height, std::move(next),
                           kSelectionReplace, changed)) {
    return fail(COMP_ERR_INTERNAL, "grow could not merge the coverage");
  }
  return COMP_OK;
}

comp_result selection_similar(SelectionState* selection, int32_t canvas_width,
                              int32_t canvas_height, const Document& document, int32_t tolerance,
                              bool* changed, std::string* error) {
  if (changed != nullptr) *changed = false;
  const auto fail = [error](comp_result code, const char* text) {
    if (error != nullptr) *error = text;
    return code;
  };

  ImageBuffer composite;
  ImageBuffer scratch;
  const std::vector<uint8_t>* pixels = nullptr;
  size_t stride = 0;
  bool ready = false;
  const comp_result code =
      grow_similar_prepare(selection, canvas_width, canvas_height, document, tolerance, "similar",
                           &pixels, &stride, &composite, &scratch, &ready, error);
  if (code != COMP_OK) return code;
  if (!ready) return COMP_OK;

  const size_t pixel_count =
      static_cast<size_t>(canvas_width) * static_cast<size_t>(canvas_height);
  const std::vector<uint8_t>& base = selection->base.pixel_data();

  std::vector<uint8_t> volume;
  size_t seeds = 0;
  /* tolerance = 255 时所有颜色都满足（RGB 每通道最大差恒 <= 255），可直接等价
   * 全选，省掉 16 MiB 占位表与三次 3D 扫描。 */
  if (tolerance < 255) {
    volume.assign(kColorVolumeCells, 0);
    for (int32_t y = 0; y < canvas_height; ++y) {
      const uint8_t* row = pixels->data() + static_cast<size_t>(y) * stride;
      const size_t row_offset = static_cast<size_t>(y) * static_cast<size_t>(canvas_width);
      for (int32_t x = 0; x < canvas_width; ++x) {
        if (base[row_offset + static_cast<size_t>(x)] == 0) continue;
        volume[color_key(row + static_cast<size_t>(x) * 4)] = 1;
        ++seeds;
      }
    }
  } else {
    for (size_t i = 0; i < pixel_count; ++i) {
      if (base[i] != 0) {
        ++seeds;
        break;
      }
    }
  }
  if (seeds == 0) return COMP_OK;

  if (tolerance < 255) dilate_color_volume(&volume, tolerance);

  /* 原有蒙版逐字节保留（抗锯齿边缘不被抹平），新并入的像素写 255。 */
  std::vector<uint8_t> next(base);
  for (int32_t y = 0; y < canvas_height; ++y) {
    const uint8_t* row = pixels->data() + static_cast<size_t>(y) * stride;
    const size_t row_offset = static_cast<size_t>(y) * static_cast<size_t>(canvas_width);
    for (int32_t x = 0; x < canvas_width; ++x) {
      const size_t index = row_offset + static_cast<size_t>(x);
      if (next[index] != 0) continue;
      const bool match =
          tolerance >= 255 || volume[color_key(row + static_cast<size_t>(x) * 4)] != 0;
      if (match) next[index] = 255;
    }
  }

  /* 逐字节相同就不落步（apply_coverage_mask 自己也会判一次，这里提前省掉
   * 一次 16 MiB 的比较与羽化归零的副作用判断）。 */
  if (masks_equal(next, base)) return COMP_OK;
  if (!apply_coverage_mask(selection, canvas_width, canvas_height, std::move(next),
                           kSelectionReplace, changed)) {
    return fail(COMP_ERR_INTERNAL, "similar could not merge the coverage");
  }
  return COMP_OK;
}

void blend_through_selection(uint8_t* rgba, size_t stride, const uint8_t* original, int32_t width,
                             int32_t height, const std::vector<uint8_t>& coverage,
                             const Layer& layer, int32_t canvas_width, int32_t canvas_height) {
  if (coverage.empty() || canvas_width <= 0 || canvas_height <= 0) {
    /* 没有有效 coverage：整层保持原样（显式空选区在上游就是「什么都不改」）。 */
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(rgba + static_cast<size_t>(y) * stride,
                  original + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
    }
    return;
  }
  const comp_rect bounds = layer.effective_bounds();
  /* 轴对齐快路径：整数平移与旧实现逐字相同，保证既有用例逐字节不变。
   * 一旦有旋转/翻转，或包围盒与像素尺寸不一致（缩放），图层像素网格与文档
   * 网格不再同构，必须走正变换逐点取样。 */
  const bool axis_aligned = static_cast<double>(layer.rotation) == 0.0 && !layer.flip_x &&
                            !layer.flip_y && bounds.w == width && bounds.h == height;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = rgba + static_cast<size_t>(y) * stride;
    const uint8_t* before = original + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4;
      const uint8_t* o = before + static_cast<size_t>(x) * 4;
      int32_t doc_x = 0;
      int32_t doc_y = 0;
      bool mapped = true;
      if (axis_aligned) {
        doc_x = bounds.x + x;
        doc_y = bounds.y + y;
      } else {
        double fx = 0.0;
        double fy = 0.0;
        mapped = layer.pixel_to_document(width, height, x, y, &fx, &fy);
        if (mapped) {
          /* coverage 是硬边蒙版，采样用最近邻（与文档→图层方向同口径）。 */
          doc_x = static_cast<int32_t>(std::lround(fx));
          doc_y = static_cast<int32_t>(std::lround(fy));
        }
      }
      int32_t cov = 0;
      if (mapped && doc_x >= 0 && doc_y >= 0 && doc_x < canvas_width && doc_y < canvas_height) {
        cov = coverage[static_cast<size_t>(doc_y) * static_cast<size_t>(canvas_width) +
                       static_cast<size_t>(doc_x)];
      }
      if (cov == 0) {
        for (int c = 0; c < 4; ++c) p[c] = o[c];
      } else if (cov < 255) {
        for (int c = 0; c < 4; ++c) {
          const int blended = (p[c] * cov + o[c] * (255 - cov) + 127) / 255;
          p[c] = static_cast<uint8_t>(std::min(255, blended));
        }
      }
    }
  }
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

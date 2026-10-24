// AI生成
#include "compositing/effect_eval.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/comp_geometry.h"
#include "format/comp_json.h"

namespace compositor {

namespace {

/* ---------------------------------------------------------------- 参数读取
 *
 * 键名逐字取自上游 `LayerEffects.swift` 的字段名（size / red / green / blue /
 * opacity / inside / angle / distance / blur），默认值也逐字对齐，使
 * engineEffects 与上游对象模型同名同义，便于两端互通。
 */
struct Settings {
  JsonValue root;
  bool parsed = false;

  void load(const std::string& text) {
    if (text.empty()) return;
    std::string error;
    JsonValue value = json_parse(text, &error);
    if (!value.is_object() || !error.empty()) return;
    root = std::move(value);
    parsed = true;
  }
  double number(const char* key, double fallback) const {
    const JsonValue* node = root.find(key);
    if (node == nullptr || !node->is_number()) return fallback;
    return node->as_number(fallback);
  }
  bool boolean(const char* key, bool fallback) const {
    const JsonValue* node = root.find(key);
    if (node == nullptr || !node->is_bool()) return fallback;
    return node->as_bool(fallback);
  }
  double clamp(double v, double lo, double hi) const { return v < lo ? lo : (v > hi ? hi : v); }
  /* 上游 isValid 的范围；越界值钳回而非拒绝，理由见头文件「参数一律钳制」。 */
  double unit(const char* key, double fallback) const { return clamp(number(key, fallback), 0.0, 1.0); }
  double ranged(const char* key, double fallback, double lo, double hi) const {
    return clamp(number(key, fallback), lo, hi);
  }
};

/* 直通色（0..1）；上游 PaletteColor 由 red/green/blue 三个 0…1 量构成。 */
struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

Rgb read_color(const Settings& s) {
  Rgb c;
  c.r = s.unit("red", 0.0);
  c.g = s.unit("green", 0.0);
  c.b = s.unit("blue", 0.0);
  return c;
}

uint8_t to_u8(double v) {
  if (v <= 0.0) return 0;
  if (v >= 255.0) return 255;
  return static_cast<uint8_t>(v + 0.5);
}

size_t pixel_count(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) return 0;
  return static_cast<size_t>(width) * static_cast<size_t>(height);
}

/* ------------------------------------------------------------ 覆盖率灰度图
 *
 * 上游的 coverage 是一张灰度图（0…255），由形状的 alpha 放置 + 可选高斯模糊得到
 * （LayerEffects.swift:588 `coverage(_:in:size:blur:)`）。本引擎用同样的表达：
 * 一张与画布同尺寸的 uint8 灰度缓冲。全程整数运算，避免同一张 coverage 被两条
 * 效果复用时因浮点路径不同而出现缝隙。
 */
struct GrayMap {
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> data;

  void resize(int32_t w, int32_t h) {
    width = w;
    height = h;
    data.assign(pixel_count(w, h), 0);
  }
  uint8_t& at(int32_t x, int32_t y) { return data[static_cast<size_t>(y) * width + x]; }
  uint8_t at(int32_t x, int32_t y) const { return data[static_cast<size_t>(y) * width + x]; }
  bool empty() const { return data.empty(); }
};

/* 取图层「呈现出来的 alpha」。
 *
 * 本函数作用于已含变换与蒙版的图层单元缓冲（build_layer_unit 的产物，文档坐标）。
 * 上游 masked(image, mask:) 先把蒙版乘进像素再做效果，效果因此跟随实际呈现的
 * 形状；本引擎的单元缓冲已含蒙版，直接取 alpha 即等价。 */
void shape_alpha_from(const uint8_t* buf, int32_t width, int32_t height, size_t stride,
                      GrayMap* out) {
  out->resize(width, height);
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* row = buf + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      out->at(x, y) = row[static_cast<size_t>(x) * 4 + 3];
    }
  }
}

/* 把灰度图按 offset 平移（超出范围处为 0）——上游 `placed.offsetBy(dx:dy:)`。 */
GrayMap shifted(const GrayMap& src, double dx, double dy) {
  GrayMap out;
  out.resize(src.width, src.height);
  const int32_t shift_x = static_cast<int32_t>(std::lround(dx));
  const int32_t shift_y = static_cast<int32_t>(std::lround(dy));
  for (int32_t y = 0; y < src.height; ++y) {
    const int32_t sy = y - shift_y;
    if (sy < 0 || sy >= src.height) continue;
    for (int32_t x = 0; x < src.width; ++x) {
      const int32_t sx = x - shift_x;
      if (sx < 0 || sx >= src.width) continue;
      out.at(x, y) = src.at(sx, sy);
    }
  }
  return out;
}

/*
 * 三趟滑动窗口盒式高斯（与 adjustment_eval 的 apply_gaussian_blur 同一近似）。
 *
 * 上游用 Core Image 的 `applyingGaussianBlur(sigma: blur / 2)`；本引擎没有 Core
 * Image，复用调整求值里已钉住并有逐字节护栏的同一近似——同一参数在两条路径上
 * 得到同一结果，避免「投影的模糊」与「高斯模糊调整」看起来不一样。
 *
 * sigma 口径：上游传的是 blur/2（blur 是 UI 上的直径量，sigma 取其一半）。
 */
void blur_gray(GrayMap* map, double blur) {
  if (map == nullptr || map->empty() || blur <= 0.0) return;
  const double sigma = blur * 0.5;
  const int passes = 3;
  const double variance = sigma * sigma * (12.0 / passes + 1.0);
  const int box = std::max(1, static_cast<int>(std::lround(std::sqrt(variance))));
  std::vector<uint8_t> temp(map->data.size(), 0);
  for (int pass = 0; pass < passes; ++pass) {
    const bool horizontal = (pass % 2) == 0;
    const int32_t outer = horizontal ? map->height : map->width;
    const int32_t inner = horizontal ? map->width : map->height;
    const uint8_t* src = map->data.data();
    uint8_t* dst = temp.data();
    for (int32_t o = 0; o < outer; ++o) {
      double sum = 0.0;
      int hits = 0;
      for (int32_t k = -box; k <= box; ++k) {
        if (k < 0 || k >= inner) continue;
        const size_t off = horizontal ? static_cast<size_t>(o) * static_cast<size_t>(inner) +
                                            static_cast<size_t>(k)
                                      : static_cast<size_t>(k) * static_cast<size_t>(inner) +
                                            static_cast<size_t>(o);
        sum += src[off];
        ++hits;
      }
      for (int32_t i = 0; i < inner; ++i) {
        const size_t off = horizontal ? static_cast<size_t>(o) * static_cast<size_t>(inner) +
                                            static_cast<size_t>(i)
                                      : static_cast<size_t>(i) * static_cast<size_t>(inner) +
                                            static_cast<size_t>(o);
        dst[off] = to_u8(sum / std::max(1, hits));
        const int32_t add = i + box + 1;
        const int32_t sub = i - box;
        if (add < inner) {
          const size_t o2 = horizontal
                                ? static_cast<size_t>(o) * static_cast<size_t>(inner) +
                                      static_cast<size_t>(add)
                                : static_cast<size_t>(add) * static_cast<size_t>(inner) +
                                      static_cast<size_t>(o);
          sum += src[o2];
          ++hits;
        }
        if (sub >= 0) {
          const size_t o2 = horizontal
                                ? static_cast<size_t>(o) * static_cast<size_t>(inner) +
                                      static_cast<size_t>(sub)
                                : static_cast<size_t>(sub) * static_cast<size_t>(inner) +
                                      static_cast<size_t>(o);
          sum -= src[o2];
          --hits;
        }
      }
    }
    map->data.swap(temp);
  }
}

/*
 * 方形扩张/收缩的滑动极值（上游 `strokeCoverage` + `extreme`）。
 *
 * reach = max(1, round(size))；outside 描边取窗口最大（形状扩张），
 * inside 描边取窗口最小（形状收缩）。两次单调队列扫描，代价不随 reach 增长。
 */
GrayMap morphological_extreme(const GrayMap& src, int32_t reach, bool smallest) {
  GrayMap out;
  out.resize(src.width, src.height);
  const int32_t radius = std::max(0, reach);
  std::vector<uint8_t> pass(pixel_count(src.width, src.height), 0);

  for (int32_t y = 0; y < src.height; ++y) {
    std::vector<int32_t> deque;
    size_t head = 0;
    int32_t next = 0;
    for (int32_t center = 0; center < src.width; ++center) {
      while (next <= std::min(src.width - 1, center + radius)) {
        const uint8_t candidate = src.at(next, y);
        while (head < deque.size()) {
          const uint8_t back = src.at(deque.back(), y);
          /* 维护单调队列：求最大值时从队尾弹出更小的，求最小值时弹出更大的。 */
          if (smallest ? (back >= candidate) : (back <= candidate)) {
            deque.pop_back();
          } else {
            break;
          }
        }
        deque.push_back(next);
        ++next;
      }
      const int32_t limit = center - radius;
      while (head < deque.size() && deque[head] < limit) ++head;
      pass[static_cast<size_t>(y) * src.width + center] = src.at(deque[head], y);
    }
  }
  for (int32_t x = 0; x < src.width; ++x) {
    std::vector<int32_t> deque;
    size_t head = 0;
    int32_t next = 0;
    for (int32_t center = 0; center < src.height; ++center) {
      while (next <= std::min(src.height - 1, center + radius)) {
        const uint8_t candidate = pass[static_cast<size_t>(next) * src.width + x];
        while (head < deque.size()) {
          const uint8_t back = pass[static_cast<size_t>(deque.back()) * src.width + x];
          if (smallest ? (back >= candidate) : (back <= candidate)) {
            deque.pop_back();
          } else {
            break;
          }
        }
        deque.push_back(next);
        ++next;
      }
      const int32_t limit = center - radius;
      while (head < deque.size() && deque[head] < limit) ++head;
      out.at(x, center) = pass[static_cast<size_t>(deque[head]) * src.width + x];
    }
  }
  return out;
}

/* 投影/内阴影的偏移量（上游 `ShadowEffect.offset`，逐字对齐其符号约定）。 */
void shadow_offset(double angle_deg, double distance, int32_t* dx, int32_t* dy) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = angle_deg * kPi / 180.0;
  /* 阴影背向光源；图层像素的 y 轴向下生长。 */
  *dx = static_cast<int32_t>(std::lround(-std::cos(radians) * distance));
  *dy = static_cast<int32_t>(std::lround(std::sin(radians) * distance));
}

/* 取反（上游多处 `max(0, min(1, 1 - level))` 的 8 位等价）。 */
GrayMap inverted(const GrayMap& src) {
  GrayMap out;
  out.resize(src.width, src.height);
  for (size_t i = 0; i < src.data.size(); ++i) {
    out.data[i] = static_cast<uint8_t>(255 - src.data[i]);
  }
  return out;
}

/* 差并夹到 [0,255]（描边环 `max(0, moved - shape)`；外发光 `soft - shape`）。 */
GrayMap subtracted(const GrayMap& a, const GrayMap& b) {
  GrayMap out;
  out.resize(a.width, a.height);
  for (size_t i = 0; i < a.data.size(); ++i) {
    out.data[i] = static_cast<uint8_t>(
        std::max(0, static_cast<int>(a.data[i]) - static_cast<int>(b.data[i])));
  }
  return out;
}

/* 逐像素乘一个系数（斜面把「深度 × 不透明度」折算进覆盖率）。 */
GrayMap scaled(const GrayMap& src, double factor) {
  GrayMap out;
  out.resize(src.width, src.height);
  if (factor <= 0.0) return out;
  for (size_t i = 0; i < src.data.size(); ++i) {
    out.data[i] = to_u8(static_cast<double>(src.data[i]) * factor);
  }
  return out;
}

/* 逐像素绝对差（缎光：两个偏移副本的 |a - b|）。 */
GrayMap abs_difference(const GrayMap& a, const GrayMap& b) {
  GrayMap out;
  out.resize(a.width, a.height);
  for (size_t i = 0; i < a.data.size(); ++i) {
    out.data[i] = static_cast<uint8_t>(
        std::abs(static_cast<int>(a.data[i]) - static_cast<int>(b.data[i])));
  }
  return out;
}

/* 形状（alpha > 0）的包围盒；空形状返回 false。渐变叠加用它把投影归一化到形状上。 */
bool shape_bounds(const GrayMap& shape, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
  int32_t min_x = shape.width;
  int32_t min_y = shape.height;
  int32_t max_x = -1;
  int32_t max_y = -1;
  for (int32_t y = 0; y < shape.height; ++y) {
    for (int32_t x = 0; x < shape.width; ++x) {
      if (shape.at(x, y) == 0) continue;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
      if (y < min_y) min_y = y;
      if (y > max_y) max_y = y;
    }
  }
  if (max_x < 0 || max_y < 0) return false;
  *x0 = min_x;
  *y0 = min_y;
  *x1 = max_x;
  *y1 = max_y;
  return true;
}

/* 环绕到 [-180,180]，让 350° 与 -10° 等价。 */
double wrap_degrees(double degrees) {
  if (!std::isfinite(degrees)) return 0.0;
  double wrapped = std::fmod(degrees, 360.0);
  if (wrapped > 180.0) wrapped -= 360.0;
  if (wrapped < -180.0) wrapped += 360.0;
  return wrapped;
}

/*
 * 光向量（屏幕坐标，y 轴向下）：angle 是光来的方向，0° = 从右来、90° = 从上来。
 * altitude 压扁垂直分量（0° = 正侧面、90° = 正上方），与上游 Emboss 的
 * 「角度 / 高度」两参数同义。返回值已四舍五入到整像素，供 shifted() 使用。
 */
void light_vector(double angle_deg, double altitude_deg, double reach, int32_t* dx,
                  int32_t* dy) {
  constexpr double kPi = 3.14159265358979323846;
  const double radians = wrap_degrees(angle_deg) * kPi / 180.0;
  double altitude = wrap_degrees(altitude_deg);
  if (altitude < 0.0) altitude = 0.0;
  if (altitude > 90.0) altitude = 90.0;
  const double flat = std::cos(altitude * kPi / 180.0);
  *dx = static_cast<int32_t>(std::lround(std::cos(radians) * reach * flat));
  *dy = static_cast<int32_t>(std::lround(-std::sin(radians) * reach * flat));
}

/*
 * 以 coverage 为裁剪铺一层纯色（上游 `fill(color, alpha:coverage:...)`）。
 *
 * 只写 coverage > 0 且 alpha > 0 的像素，因此**不改动**这些像素之外的任何值——
 * 这正是「先铺 outside 效果、再把图层像素压上去」得以成立的前提。
 */
void fill_coverage(uint8_t* buf, int32_t width, int32_t height, size_t stride,
                   const GrayMap& coverage, const Rgb& color, double alpha) {
  if (alpha <= 0.0) return;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = buf + static_cast<size_t>(y) * stride;
    const uint8_t* cov = coverage.data.data() + static_cast<size_t>(y) * coverage.width;
    for (int32_t x = 0; x < width; ++x) {
      const double a = (static_cast<double>(cov[x]) / 255.0) * alpha;
      if (a <= 0.0) continue;
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      const double dst_alpha = static_cast<double>(px[3]) / 255.0;
      const double out_alpha = a + dst_alpha * (1.0 - a);
      if (out_alpha <= 0.0) continue;
      const double keep = 1.0 - a;
      for (int c = 0; c < 3; ++c) {
        const double src_channel = c == 0 ? color.r : (c == 1 ? color.g : color.b);
        const double out_premul = src_channel * a +
                                  (static_cast<double>(px[c]) / 255.0) * keep;
        px[c] = to_u8(out_premul * 255.0);
      }
      px[3] = to_u8(out_alpha * 255.0);
    }
  }
}

/*
 * source-atop 叠色（上游 `recolor(_:alpha:amount:in:)`）。
 *
 * source-atop：只落在已有 alpha 之内，且**不改变 alpha**（半透明像素完全吃到
 * 颜色，正是上游注释强调的语义）。amount 为 null 时全图满强度（Color Overlay），
 * 非 null 时按该 coverage 逐像素衰减（内发光/内阴影）。
 */
void recolor_atop(uint8_t* buf, int32_t width, int32_t height, size_t stride,
                  const Rgb& color, double alpha, const GrayMap* amount) {
  if (alpha <= 0.0) return;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = buf + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      const uint8_t a = px[3];
      if (a == 0) continue;
      double a_eff = alpha;
      if (amount != nullptr) {
        a_eff *= static_cast<double>(amount->at(x, y)) / 255.0;
        if (a_eff <= 0.0) continue;
      }
      /* a_denom 是 0…255 的 alpha 值；px[c] 也是 0…255 的预乘值。 */
      const double a_denom = static_cast<double>(a);
      for (int c = 0; c < 3; ++c) {
        /*
         * px[c] 是**预乘**值（0…255），要得到 0…1 的直通色必须先除以 alpha
         * （a_denom 同样是 0…255，两者同量纲，比值即 0…1 的直通色）。
         * 直接把预乘值当直通色用，等于在 alpha<1 的像素上把颜色算暗 alpha 倍。
         *
         * 半透明像素正是 source-atop 的意义所在（上游注释：半透明像素完全吃到颜色），
         * 所以这条必须对：alpha=128 的像素满覆盖叠红后应是 r=128（预乘红），
         * 若不 unpremultiply 会得到 r=64，发暗一半。
         */
        const double base_straight = static_cast<double>(px[c]) / a_denom;
        const double src_channel = c == 0 ? color.r : (c == 1 ? color.g : color.b);
        const double straight = base_straight * (1.0 - a_eff) + src_channel * a_eff;
        /* 量化误差可能让 base_straight 略超 1，先钳到 0…1 再乘回 0…255 的 alpha。
         * 这里**不能**再乘一个 255：clamped 是 0…1，a_denom 是 0…255，
         * 两者相乘已经落在 0…255，再乘 255 会被 to_u8 夹成 255（红像素洗成纯红）。 */
        const double clamped = straight < 0.0 ? 0.0 : (straight > 1.0 ? 1.0 : straight);
        px[c] = to_u8(clamped * a_denom);
      }
    }
  }
}

/*
 * 以双色线性斜坡做 source-atop（渐变叠加）。
 *
 * 归一化口径：把**源形状包围盒**的四个角投影到单位方向 u 上，取最小/最大值作为
 * 0 与 1。这样无论画布多大、图层落在哪，渐变都铺满形状本身——与上游「渐变覆盖
 * 图层边界」一致，也不会因为空画布留白而把整段渐变挤到一小块里。
 *
 * 与 recolor_atop 同一套预乘/反预乘与不改变 alpha 的规则；区别只是每个像素先按
 * 自己的投影位置插值出源色，而不是全图一色。
 */
void recolor_atop_gradient(uint8_t* buf, int32_t width, int32_t height, size_t stride,
                           const GrayMap& shape, const Rgb& from, const Rgb& to,
                           double angle_deg, bool reverse, double alpha) {
  if (alpha <= 0.0) return;
  int32_t x0 = 0;
  int32_t y0 = 0;
  int32_t x1 = 0;
  int32_t y1 = 0;
  if (!shape_bounds(shape, &x0, &y0, &x1, &y1)) return;
  constexpr double kPi = 3.14159265358979323846;
  const double radians = wrap_degrees(angle_deg) * kPi / 180.0;
  const double ux = std::cos(radians);
  const double uy = std::sin(radians); /* 屏幕坐标 y 向下：90° = 自上而下 */
  double lo = static_cast<double>(x0) * ux + static_cast<double>(y0) * uy;
  double hi = lo;
  const double corners[3][2] = {{static_cast<double>(x0), static_cast<double>(y1)},
                                {static_cast<double>(x1), static_cast<double>(y0)},
                                {static_cast<double>(x1), static_cast<double>(y1)}};
  for (const auto& corner : corners) {
    const double t = corner[0] * ux + corner[1] * uy;
    lo = std::min(lo, t);
    hi = std::max(hi, t);
  }
  const double span = hi - lo;
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = buf + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      const uint8_t a = px[3];
      if (a == 0) continue;
      double t = span > 0.0
                     ? (static_cast<double>(x) * ux + static_cast<double>(y) * uy - lo) / span
                     : 0.0;
      if (t < 0.0) t = 0.0;
      if (t > 1.0) t = 1.0;
      if (reverse) t = 1.0 - t;
      const Rgb color{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t,
                      from.b + (to.b - from.b) * t};
      const double a_denom = static_cast<double>(a);
      for (int c = 0; c < 3; ++c) {
        const double base_straight = static_cast<double>(px[c]) / a_denom;
        const double src_channel = c == 0 ? color.r : (c == 1 ? color.g : color.b);
        const double straight = base_straight * (1.0 - alpha) + src_channel * alpha;
        const double clamped = straight < 0.0 ? 0.0 : (straight > 1.0 ? 1.0 : straight);
        px[c] = to_u8(clamped * a_denom);
      }
    }
  }
}

/* ------------------------------------------------------------------ 逐效果
 *
 * 每个效果被拆成「规划」与「落笔」两步：规划只读参数、决定它落在像素之下
 * （under）还是像素之上（over）以及是否可见；落笔才写像素。
 *
 * 之所以不能合并成一步：一个效果只会执行一次，而每个效果要么是 under 要么是
 * over。若在 under 那一趟里「先画了再判断画的是哪一侧」，落在 over 侧的效果
 * 会被画进 under 画布，随后又在 over 画布上画一次——叠色类效果因此出现两次
 * （source-atop 幂等所以看不出颜色错，但计数与像素都对不上），投影类则会把
 * 像素之下垫出本该压在像素之上的东西。规划在前，一趟只画自己那一侧。
 */
enum class DrawKind { kNone = 0, kUnder, kOver };

/* 规划结果：参数已解析好，落笔时不必再解 JSON。 */
struct EffectPlan {
  uint32_t effect = 0;
  DrawKind kind = DrawKind::kNone;
  double angle = 0.0;
  double distance = 0.0;
  double blur = 0.0;
  double size = 0.0;
  double opacity = 0.0;
  Rgb color;
  bool inside = false;
  /* P1-1：斜面/缎光/渐变叠加追加的字段。 */
  double altitude = 0.0;      /* 斜面的光照高度角（度） */
  double depth = 100.0;       /* 斜面的强度百分比 */
  double soften = 0.0;        /* 斜面的额外柔化（px） */
  bool invert = false;        /* 缎光取反 */
  bool reverse = false;       /* 渐变叠加反向 */
  Rgb color2;                 /* 渐变终点色 / 图案底色 */
  Rgb shadow_color;           /* 斜面暗面色 */
  /* P1-1 第二段：图案叠加追加的字段。 */
  int32_t pattern = 0;        /* 内置图案 0..3（棋盘/网格/斜纹/圆点） */
  double pattern_scale = 100.0; /* 图案缩放百分比，100 = 32px 周期 */
  double offset_x = 0.0;      /* 图案平移（文档像素） */
  double offset_y = 0.0;
};

/*
 * 图案叠加（P1-1 第二段）。
 *
 * 图案是**程序化内置**的：没有资源文件、没有随机数，坐标先平移、再按 angle
 * 旋转、最后按 scale 缩放，以 32px 为 100% 基准周期平铺。四种图案：
 *   0 棋盘   —— 格子坐标 (cu + cv) 的奇偶交替；
 *   1 网格   —— 每格左边/上边各 1px 的线；
 *   2 斜纹   —— (u + v) 方向的 45° 条带；
 *   3 圆点   —— 每格中心一个半径 1/4 格的点。
 * 与 PS 的「图案预设 + 自定义图案」不同，本仓暂不支持外部图案资源（见 BACKLOG
 * P1-1 剩余项），因此这里只保证「内置图案可平铺、可旋转、可缩放、可偏移」。
 */

/* 把 value 环绕到 [0, period)；fmod 对负数保留符号，显式补齐。 */
double wrap_period(double value, double period) {
  double r = std::fmod(value, period);
  if (r < 0.0) r += period;
  return r;
}

/* ru/rv 是已经平移、旋转过的图案坐标；tile 是当前缩放下的图案周期。 */
bool pattern_covers(int32_t pattern, double ru, double rv, double tile) {
  switch (pattern) {
    case 1: /* 网格：格子的左/上边界各 1px。 */
      return wrap_period(ru, tile) < 1.0 || wrap_period(rv, tile) < 1.0;
    case 2: /* 斜纹：沿 (1,1) 方向的半周期条带。 */
      return wrap_period(ru + rv, tile * 2.0) < tile;
    case 3: { /* 圆点：格心半径 1/4 格。 */
      const double du = wrap_period(ru, tile) - tile * 0.5;
      const double dv = wrap_period(rv, tile) - tile * 0.5;
      return du * du + dv * dv <= tile * tile * 0.0625;
    }
    default: { /* 0 = 棋盘；越界值在解析处夹到 0..3，这里按 0 处理。 */
      const double cu = std::floor(ru / tile);
      const double cv = std::floor(rv / tile);
      const long long parity =
          static_cast<long long>(cu) + static_cast<long long>(cv);
      return (parity % 2) != 0;
    }
  }
}

/*
 * 以双色图案做 source-atop（图案叠加）。
 *
 * 与 recolor_atop / recolor_atop_gradient 同一套预乘与不改变 alpha 的规则；
 * 区别只是每个像素按自己的图案坐标取前景色或底色。
 */
void recolor_atop_pattern(uint8_t* buf, int32_t width, int32_t height, size_t stride,
                          const EffectPlan& plan, double alpha) {
  if (alpha <= 0.0) return;
  constexpr double kPi = 3.14159265358979323846;
  const double radians = wrap_degrees(plan.angle) * kPi / 180.0;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  const double tile = std::max(1.0, 32.0 * plan.pattern_scale / 100.0);
  for (int32_t y = 0; y < height; ++y) {
    uint8_t* row = buf + static_cast<size_t>(y) * stride;
    const double v = static_cast<double>(y) - plan.offset_y;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t* px = row + static_cast<size_t>(x) * 4;
      const uint8_t a = px[3];
      if (a == 0) continue;
      const double u = static_cast<double>(x) - plan.offset_x;
      const double ru = u * c + v * s;
      const double rv = -u * s + v * c;
      const Rgb color = pattern_covers(plan.pattern, ru, rv, tile) ? plan.color : plan.color2;
      const double a_denom = static_cast<double>(a);
      for (int ch = 0; ch < 3; ++ch) {
        const double base_straight = static_cast<double>(px[ch]) / a_denom;
        const double src_channel = ch == 0 ? color.r : (ch == 1 ? color.g : color.b);
        const double straight = base_straight * (1.0 - alpha) + src_channel * alpha;
        const double clamped = straight < 0.0 ? 0.0 : (straight > 1.0 ? 1.0 : straight);
        px[ch] = to_u8(clamped * a_denom);
      }
    }
  }
}

/*
 * 规划一个效果。可见性门槛逐条对齐上游 render()：
 * 投影/内阴影要求 opacity>0 且（距离或模糊）非零；外发光要求 size>0；
 * 内发光要求 size>0；叠色要求 opacity>0；描边要求 size>0 且 opacity>0。
 * 门槛不过返回 kNone（合法但不可见，不计入 applied、不改变像素）。
 */
DrawKind plan_effect(uint32_t effect, const Settings& s, EffectPlan* plan) {
  *plan = EffectPlan{};
  plan->effect = effect;
  switch (effect) {
    case kEffectDropShadow: {
      /* 上游 isValid 范围：angle ∈ [-360,360]、distance ∈ [0,5000]、blur ∈ [0,500]。 */
      plan->angle = s.ranged("angle", 90.0, -360.0, 360.0);
      plan->distance = s.ranged("distance", 20.0, 0.0, 5000.0);
      plan->blur = s.ranged("blur", 20.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 0.5);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0 || (plan->distance <= 0.0 && plan->blur <= 0.0)) return DrawKind::kNone;
      plan->kind = DrawKind::kUnder;
      return plan->kind;
    }

    case kEffectOuterGlow: {
      /* 上游 isValid 范围：size ∈ [0,500]。 */
      plan->size = s.ranged("size", 20.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 0.75);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0 || plan->size <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kUnder;
      return plan->kind;
    }

    case kEffectColorOverlay: {
      /* 上游 ColorOverlay 默认红=0 绿=0 蓝=0 不透明度=1 ⇒ 纯黑满覆盖，明显可见。 */
      plan->opacity = s.unit("opacity", 1.0);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectInnerGlow: {
      plan->size = s.ranged("size", 10.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 0.75);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0 || plan->size <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectInnerShadow: {
      plan->angle = s.ranged("angle", 90.0, -360.0, 360.0);
      plan->distance = s.ranged("distance", 10.0, 0.0, 5000.0);
      plan->blur = s.ranged("blur", 10.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 0.5);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0 || (plan->distance <= 0.0 && plan->blur <= 0.0)) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectStroke: {
      /* 上游 StrokeEffect.maxSize = 500。 */
      plan->size = s.ranged("size", 4.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 1.0);
      plan->inside = s.boolean("inside", false);
      plan->color = read_color(s);
      if (plan->size <= 0.0 || plan->opacity <= 0.0) return DrawKind::kNone;
      /* outside 垫在像素之下（否则被像素盖住）；inside 压在像素之上。 */
      plan->kind = plan->inside ? DrawKind::kOver : DrawKind::kUnder;
      return plan->kind;
    }

    /* ---------------------------------------------------- P1-1 追加（inside） */

    case kEffectBevel: {
      /* 上游 Bevel & Emboss 的量程：angle [-360,360]、altitude [0,90]、
       * depth [0,1000]、size [0,500]、soften [0,500]。 */
      plan->angle = s.ranged("angle", 120.0, -360.0, 360.0);
      plan->altitude = s.ranged("altitude", 30.0, 0.0, 90.0);
      plan->depth = s.ranged("depth", 100.0, 0.0, 1000.0);
      plan->size = s.ranged("size", 10.0, 0.0, 500.0);
      plan->soften = s.ranged("soften", 0.0, 0.0, 500.0);
      plan->opacity = s.unit("opacity", 0.75);
      /* 高光色缺省白、暗面色缺省黑（与 PS 的斜面默认一致）。 */
      plan->color = Rgb{s.unit("red", 1.0), s.unit("green", 1.0), s.unit("blue", 1.0)};
      plan->shadow_color = Rgb{s.unit("shadow_red", 0.0), s.unit("shadow_green", 0.0),
                               s.unit("shadow_blue", 0.0)};
      /* size 为 0 时没有斜面宽度可言；depth 为 0 时亮度差恒为 0。 */
      if (plan->opacity <= 0.0 || plan->size <= 0.0 || plan->depth <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectSatin: {
      plan->angle = s.ranged("angle", 19.0, -360.0, 360.0);
      plan->distance = s.ranged("distance", 11.0, 0.0, 5000.0);
      plan->blur = s.ranged("blur", 14.0, 0.0, 500.0);
      plan->invert = s.boolean("invert", false);
      plan->opacity = s.unit("opacity", 0.5);
      plan->color = read_color(s);
      if (plan->opacity <= 0.0 || plan->distance <= 0.0) {
        return DrawKind::kNone;
      }
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectGradientOverlay: {
      plan->angle = s.ranged("angle", 90.0, -360.0, 360.0);
      plan->reverse = s.boolean("reverse", false);
      plan->opacity = s.unit("opacity", 1.0);
      plan->color = read_color(s); /* 起点色，缺省黑 */
      plan->color2 = Rgb{s.unit("red2", 1.0), s.unit("green2", 1.0), s.unit("blue2", 1.0)};
      if (plan->opacity <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    case kEffectPatternOverlay: {
      /* 图案编号越界由 ranged 夹到 0..3（面板量程同值），再四舍五入成整数。 */
      plan->pattern =
          static_cast<int32_t>(std::lround(s.ranged("pattern", 0.0, 0.0, 3.0)));
      plan->pattern_scale = s.ranged("scale", 100.0, 10.0, 1000.0);
      plan->angle = s.ranged("angle", 0.0, -360.0, 360.0);
      plan->offset_x = s.ranged("offset_x", 0.0, -5000.0, 5000.0);
      plan->offset_y = s.ranged("offset_y", 0.0, -5000.0, 5000.0);
      plan->opacity = s.unit("opacity", 1.0);
      plan->color = read_color(s); /* 图案「有」的一格，缺省黑 */
      plan->color2 = Rgb{s.unit("red2", 1.0), s.unit("green2", 1.0), s.unit("blue2", 1.0)};
      if (plan->opacity <= 0.0) return DrawKind::kNone;
      plan->kind = DrawKind::kOver;
      return plan->kind;
    }

    default:
      return DrawKind::kNone;
  }
}

/* 按规划落笔（target 为整幅文档坐标画布，stride 为其行字节数）。 */
void draw_planned(const EffectPlan& plan, const GrayMap& shape, uint8_t* target,
                  int32_t width, int32_t height, size_t stride) {
  switch (plan.effect) {
    case kEffectDropShadow: {
      int32_t dx = 0;
      int32_t dy = 0;
      shadow_offset(plan.angle, plan.distance, &dx, &dy);
      GrayMap coverage = shape;
      if (dx != 0 || dy != 0) coverage = shifted(shape, dx, dy);
      if (plan.blur > 0.0) blur_gray(&coverage, plan.blur);
      fill_coverage(target, width, height, stride, coverage, plan.color, plan.opacity);
      break;
    }
    case kEffectOuterGlow: {
      GrayMap soft = shape;
      blur_gray(&soft, plan.size);
      /* 外发光 = 柔化后的形状扣掉形状内部（上游 `levels * (1 - mask)`）。 */
      fill_coverage(target, width, height, stride, subtracted(soft, shape), plan.color,
                    plan.opacity);
      break;
    }
    case kEffectColorOverlay:
      recolor_atop(target, width, height, stride, plan.color, plan.opacity, nullptr);
      break;
    case kEffectInnerGlow: {
      GrayMap soft = shape;
      blur_gray(&soft, plan.size);
      /* 内发光 = 柔化后取逆（边缘最强、向内衰减），再 source-atop 叠色。 */
      const GrayMap amount = inverted(soft);
      recolor_atop(target, width, height, stride, plan.color, plan.opacity, &amount);
      break;
    }
    case kEffectInnerShadow: {
      int32_t dx = 0;
      int32_t dy = 0;
      shadow_offset(plan.angle, plan.distance, &dx, &dy);
      GrayMap moved = shape;
      if (dx != 0 || dy != 0) moved = shifted(shape, dx, dy);
      if (plan.blur > 0.0) blur_gray(&moved, plan.blur);
      /* 内阴影 = 平移柔化后取逆（落在形状内部的部分），再 source-atop 叠色。 */
      const GrayMap amount = inverted(moved);
      recolor_atop(target, width, height, stride, plan.color, plan.opacity, &amount);
      break;
    }
    case kEffectStroke: {
      const int32_t reach = std::max(1, static_cast<int32_t>(std::lround(plan.size)));
      /*
       * 两种方向的环是**不同**的减法，不能共用一个顺序：
       *   outside：扩张后的形状 减去 原形状   => max(0, grow - shape)，即外圈；
       *   inside ：原形状      减去 收缩后的形状 => max(0, shape - erode)，即内圈。
       *
       * 此前两边都写成 subtracted(grow, shape)：outside 恰好对，inside 则算成
       * 「收缩后的形状 - 原形状」，而收缩是原形状的子集，逐像素恒为负 => 夹到 0
       * => inside 描边完全画不出来（无声无息地不生效）。
       * 这正是本组护栏里 effect_eval_stroke_inside_* 要钉住的那一条。
       */
      const GrayMap moved = morphological_extreme(shape, reach, plan.inside);
      const GrayMap ring = plan.inside ? subtracted(shape, moved) : subtracted(moved, shape);
      fill_coverage(target, width, height, stride, ring, plan.color, plan.opacity);
      break;
    }
    /* -------------------------------------------- P1-1 追加（均只在形状内部） */
    case kEffectBevel: {
      /*
       * 斜面 = 「朝光平移的柔化形状」减「背光平移的柔化形状」：
       * 差值 > 0 的一侧朝向光源（高光），< 0 的一侧背光（暗面）。
       * 用平移差分而不是逐像素梯度核，是为了复用既有 shifted()/subtracted()，
       * 且 size 同时决定斜面宽度（平移量）与柔化范围，与 PS 的手感一致。
       */
      GrayMap soft = shape;
      blur_gray(&soft, plan.size);
      const double reach = std::max(1.0, plan.size * 0.5);
      int32_t dx = 0;
      int32_t dy = 0;
      light_vector(plan.angle, plan.altitude, reach, &dx, &dy);
      /* 光正对镜头（高度角 90° 且 reach 被压成 0）时没有方向差，效果不可见。 */
      if (dx == 0 && dy == 0) break;
      const GrayMap toward = shifted(soft, dx, dy);
      const GrayMap away = shifted(soft, -dx, -dy);
      GrayMap highlight = subtracted(toward, away);
      GrayMap shadow = subtracted(away, toward);
      if (plan.soften > 0.0) {
        blur_gray(&highlight, plan.soften);
        blur_gray(&shadow, plan.soften);
      }
      const double gain = plan.depth / 100.0 * plan.opacity;
      const GrayMap shadow_amount = scaled(shadow, gain);
      const GrayMap highlight_amount = scaled(highlight, gain);
      recolor_atop(target, width, height, stride, plan.shadow_color, 1.0, &shadow_amount);
      recolor_atop(target, width, height, stride, plan.color, 1.0, &highlight_amount);
      break;
    }
    case kEffectSatin: {
      /* 缎光 = 正反两个偏移副本之差（或取反）柔化后叠色。 */
      const double reach = std::max(1.0, plan.distance);
      int32_t dx = 0;
      int32_t dy = 0;
      /* 缎光只有方向与距离，没有高度角：altitude 传 0 让偏移量全额生效。 */
      light_vector(plan.angle, 0.0, reach, &dx, &dy);
      const GrayMap forward = shifted(shape, dx, dy);
      const GrayMap backward = shifted(shape, -dx, -dy);
      GrayMap band = abs_difference(forward, backward);
      if (plan.blur > 0.0) blur_gray(&band, plan.blur);
      if (plan.invert) band = inverted(band);
      recolor_atop(target, width, height, stride, plan.color, plan.opacity, &band);
      break;
    }
    case kEffectGradientOverlay:
      recolor_atop_gradient(target, width, height, stride, shape, plan.color, plan.color2,
                            plan.angle, plan.reverse, plan.opacity);
      break;
    case kEffectPatternOverlay:
      recolor_atop_pattern(target, width, height, stride, plan, plan.opacity);
      break;
    default:
      break;
  }
}

}  // namespace

bool effect_kind_is_evaluated(uint32_t effect) { return effect < kEffectKindCount; }

const char* const kUnevaluatedEffectMarker = "effect kind this engine does not evaluate";

/*
 * 效果需要额外多少文档空间（上游 `LayerEffectsRenderer.margin(for:)`）。
 *
 * outside 描边吃 size；投影吃 distance + blur*3；外发光吃 size*3；最后 ceil + 2。
 * 宿主用它把图层包围盒外扩，否则 outside 效果会被 bounds 裁掉。
 * 无可见效果（opacity 为 0 / size 为 0）返回 0。
 */
int32_t effects_margin(const std::vector<Effect>& effects) {
  double margin = 0.0;
  for (const Effect& effect : effects) {
    Settings s;
    s.load(effect.settings_json);
    const double opacity = s.unit("opacity", 1.0);
    switch (effect.effect) {
      case kEffectStroke: {
        if (s.boolean("inside", false)) break; /* inside 不外扩 */
        margin = std::max(margin, s.ranged("size", 4.0, 0.0, 500.0));
        break;
      }
      case kEffectDropShadow: {
        margin = std::max(margin, s.ranged("distance", 20.0, 0.0, 5000.0) +
                                    s.ranged("blur", 20.0, 0.0, 500.0) * 3.0);
        break;
      }
      case kEffectOuterGlow:
        margin = std::max(margin, s.ranged("size", 20.0, 0.0, 500.0) * 3.0);
        break;
      case kEffectColorOverlay:
      case kEffectInnerShadow:
      case kEffectInnerGlow:
      case kEffectBevel:
      case kEffectSatin:
      case kEffectGradientOverlay:
      case kEffectPatternOverlay:
        break;
      default:
        break; /* 未知效果不预留空间（它也不会被绘制） */
    }
    if (opacity < 0.0) continue; /* 钳制后不会为负，此处仅为显式表达 */
  }
  if (margin <= 0.0) return 0;
  return static_cast<int32_t>(std::ceil(margin)) + 2;
}

bool apply_effects(const std::vector<Effect>& effects, uint8_t* buf, int32_t width,
                   int32_t height, size_t stride, EffectEvalResult* result,
                   std::string* error) {
  if (result != nullptr) *result = EffectEvalResult{};
  if (effects.empty()) return true;
  if (buf == nullptr || width <= 0 || height <= 0) {
    if (error != nullptr) *error = "effect target buffer is invalid";
    return false;
  }
  const size_t count = pixel_count(width, height);
  if (count == 0) return true;

  /*
   * 所有效果（含 P1-1 追加的斜面/缎光/渐变叠加）全部基于同一张「图层呈现形状」
   * （上游 masked(image, mask:) 只做一次，随后投影/发光/描边/斜面都从它派生）。
   * 变换与蒙版已烘进 buf，取 alpha 即上游那份 placedMask 的等价物。
   */
  GrayMap shape;
  shape_alpha_from(buf, width, height, stride, &shape);

  /*
   * 绘制次序（上游 LayerEffects.swift:470-505，不可交换）：
   *   垫底：投影 → 外发光 → outside 描边 → 图层像素 → 叠色/内发光/内阴影 →
   *   inside 描边。
   * 本函数拿到的是**已经画好像素**的缓冲，因此按上游次序实现为：
   *   1. 把原始像素另存一份（inner_canvas）；
   *   2. 清空 buf，在其上铺 under 类效果（此时 buf 只有背景效果）；
   *   3. 把 inner_canvas 以 source-over 压回 buf（像素盖住 under 效果，符合上游
   *      「outside 描边垫在像素之下，否则会被像素盖住」）；
   *   4. over 类效果作用在**图层自己的画布**上（而非合成结果）——这与上游一致：
   *      内发光/内阴影的 source-atop 必须以图层像素的 alpha 为界，否则会把垫在
   *      外面的投影也染色。所以对 inner_canvas 求值后再合成。
   *
   * 逐效果顺序另需说明：ABI 允许同一层叠多个同类效果（set_effect 按 effect 值
   * 去重，因此每种至多一条），上游 LayerEffects 每种也至多一条，故按「同类唯一」
   * 的前提实现；效果之间的先后以本函数固定的次序为准，不看 vector 顺序。
   */
  /* 规划全部效果（只读参数），再分两趟落笔。 */
  std::vector<EffectPlan> plans;
  plans.reserve(effects.size());
  for (const Effect& effect : effects) {
    if (effect.effect >= kEffectKindCount) {
      if (result != nullptr) ++result->skipped;
      continue;
    }
    Settings s;
    s.load(effect.settings_json);
    EffectPlan plan;
    if (plan_effect(effect.effect, s, &plan) == DrawKind::kNone) continue;
    plans.push_back(plan);
  }
  if (plans.empty()) return true;

  const size_t row_bytes = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> pixels(count * 4);
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(pixels.data() + static_cast<size_t>(y) * row_bytes,
                buf + static_cast<size_t>(y) * stride, row_bytes);
  }

  /* 第 1 趟：under 类效果铺在一张全透明的画布上。 */
  std::vector<uint8_t> under_canvas(count * 4, 0);
  for (const EffectPlan& plan : plans) {
    if (plan.kind != DrawKind::kUnder) continue;
    draw_planned(plan, shape, under_canvas.data(), width, height, row_bytes);
    if (result != nullptr) ++result->applied;
  }

  /* 第 2 趟：over 类效果作用在**图层自己的画布**上（像素 + 内侧效果）。
   * 这与上游一致：内发光/内阴影的 source-atop 必须以图层像素的 alpha 为界，
   * 否则会把垫在外面的投影与外发光也一并染色。 */
  std::vector<uint8_t> over_canvas = pixels;
  for (const EffectPlan& plan : plans) {
    if (plan.kind != DrawKind::kOver) continue;
    draw_planned(plan, shape, over_canvas.data(), width, height, row_bytes);
  }

  /* 合成：under_canvas 打底，over_canvas source-over 压上。 */
  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* under_row = under_canvas.data() + static_cast<size_t>(y) * row_bytes;
    const uint8_t* over_row = over_canvas.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t* dst = buf + static_cast<size_t>(y) * stride;
    for (int32_t x = 0; x < width; ++x) {
      const uint8_t* u = under_row + static_cast<size_t>(x) * 4;
      const uint8_t* o = over_row + static_cast<size_t>(x) * 4;
      uint8_t* d = dst + static_cast<size_t>(x) * 4;
      const double src_a = static_cast<double>(o[3]) / 255.0;
      if (src_a <= 0.0) {
        d[0] = u[0];
        d[1] = u[1];
        d[2] = u[2];
        d[3] = u[3];
        continue;
      }
      const double keep = 1.0 - src_a;
      for (int c = 0; c < 3; ++c) {
        /*
         * o[c] 与 u[c] 都是**预乘**值（0…255，已含各自的 alpha），source-over
         * 直接相加即可：`out = src + dst × (1 − src_a)`。
         *
         * 此前误把 o[c] 当直通值又乘了一次 255，于是半透明边缘的预乘颜色被放大到
         * 远超 255（被 to_u8 夹成 255），渲染出 255/255/255/255 的白边——
         * 单元缓冲里明明是图层的原色，合成一步却把它洗白了。
         */
        d[c] = to_u8(static_cast<double>(o[c]) +
                     static_cast<double>(u[c]) * keep * 255.0);
      }
      d[3] = to_u8((src_a + static_cast<double>(u[3]) / 255.0 * keep) * 255.0);
    }
  }

  /* changed：规划出至少一条效果，且合成后与原始像素确有差异（形状全透明时
   * 逐字节未变）。逐字节比对而非假定为真——「挂了效果但看不见」是合法状态，
   * 把它报成 changed 会让上层误判需要重烘。 */
  if (result != nullptr && result->applied > 0) {
    std::vector<uint8_t> after(count * 4);
    for (int32_t y = 0; y < height; ++y) {
      std::memcpy(after.data() + static_cast<size_t>(y) * row_bytes,
                  buf + static_cast<size_t>(y) * stride, row_bytes);
    }
    result->changed = after != pixels;
  }
  return true;
}

}  // namespace compositor

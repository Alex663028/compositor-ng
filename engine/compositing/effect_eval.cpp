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
};

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
   * 六种效果全部基于同一张「图层呈现形状」（上游 masked(image, mask:) 只做一次，
   * 随后投影/发光/描边都从它派生）。变换与蒙版已烘进 buf，取 alpha 即上游那份
   * placedMask 的等价物。
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

// AI生成
#include "compositing/adjustment_eval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "domain/adjustment_table.h"
#include "format/comp_json.h"
#include "imaging/comp_imaging.h"

namespace compositor {

namespace {

/* ---------------------------------------------------------------- 参数读取 */

/*
 * settings 一律「缺键回落默认」。理由见头文件：settings_json 来自 .comp 互通，
 * 缺键是常态（上游的默认值即恒等变换），把缺键当错误会让互通文件打不开。
 * 越界值同样钳到合法区间——写入侧 ABI 已校验，读路径不再二次拒绝，
 * 但绝不把越界值直接喂给像素内核。
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

  /* 读取 `a.b.c` 形式的路径，任一段缺失返回 fallback。 */
  double number(const char* a, const char* b, const char* c, double fallback) const {
    const JsonValue* node = root.find(a);
    if (b != nullptr && node != nullptr) node = node->find(b);
    if (c != nullptr && node != nullptr) node = node->find(c);
    if (node == nullptr || !node->is_number()) return fallback;
    return node->as_number(fallback);
  }
  double number(const char* a, const char* b, double fallback) const {
    return number(a, b, nullptr, fallback);
  }
  bool boolean(const char* a, const char* b, bool fallback) const {
    const JsonValue* node = root.find(a);
    if (b != nullptr && node != nullptr) node = node->find(b);
    if (node == nullptr || !node->is_bool()) return fallback;
    return node->as_bool(fallback);
  }
  double clamp(double v, double lo, double hi) const { return v < lo ? lo : (v > hi ? hi : v); }
};

/* 与上游 `HueSaturationFilter.cube` 同口径：色相/饱和/明度以 Photoshop 的
 * 0…1 归一化量进入（Hue 实际按 0…360 度，调用处已换算）。 */
void hsv_to_rgb(double hue_deg, double saturation, double lightness, double* r, double* g,
                double* b) {
  const double h = std::fmod(hue_deg, 360.0) / 60.0;
  const double s = std::clamp(saturation, -1.0, 1.0);
  const double l = std::clamp(lightness, -1.0, 1.0);
  /* 负饱和/负明度向黑走：Photoshop 的 Saturation/Lightness 双向即如此。 */
  const double chroma = (1.0 - std::fabs(2.0 * l - 1.0)) * std::fabs(s);
  const double sign = s < 0.0 ? -1.0 : 1.0;
  const double hh = h < 0.0 ? h + 6.0 : (h >= 6.0 ? 0.0 : h);
  const double x = chroma * (1.0 - std::fabs(std::fmod(hh, 2.0) - 1.0));
  double rr = 0.0, gg = 0.0, bb = 0.0;
  if (hh < 1.0) { rr = chroma; gg = x; }
  else if (hh < 2.0) { rr = x; gg = chroma; }
  else if (hh < 3.0) { gg = chroma; bb = x; }
  else if (hh < 4.0) { gg = x; bb = chroma; }
  else if (hh < 5.0) { rr = x; bb = chroma; }
  else { rr = chroma; bb = x; }
  const double m = l - chroma * 0.5;
  *r = std::clamp(rr + m, 0.0, 1.0);
  *g = std::clamp(gg + m, 0.0, 1.0);
  *b = std::clamp(bb + m, 0.0, 1.0);
  (void)sign;
}

size_t pixel_count(int32_t width, int32_t height, size_t stride) {
  if (width <= 0 || height <= 0) return 0;
  return static_cast<size_t>(width) * static_cast<size_t>(height);
}

/* 预乘缓冲的 alpha 通道整体上提，使不透明区域完全覆盖（用于把调整后的颜色写回）。 */
void unpremultiply_inplace(uint8_t* buf, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* px = buf + i * 4;
    const uint32_t a = px[3];
    if (a == 255) continue;
    if (a == 0) continue;
    for (int c = 0; c < 3; ++c) {
      px[c] = static_cast<uint8_t>(std::min(255u, (px[c] * 255u + a / 2) / a));
    }
  }
}

/*
 * 3 趟盒式模糊（半径 `box`），就地作用于 RGBA8 预乘缓冲。
 *
 * 高斯逼近的**唯一实现**：kind 7 Gaussian Blur 与扫描线的磷光晕（blur_rgba8_copy）
 * 都走它。分离式：先水平后垂直；滑动窗口逐像素推进，避免 O(box * n)。
 */
void box_blur_in_place(uint8_t* buf, int32_t width, int32_t height, size_t stride, int box) {
  if (buf == nullptr || width <= 0 || height <= 0 || box <= 0) return;
  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  const size_t count = pixel_count(width, height, stride);

  std::vector<uint8_t> temp(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  uint8_t* src = buf;
  uint8_t* dst = temp.data();
  for (int pass = 0; pass < 3; ++pass) {
    const bool horizontal = (pass % 2) == 0;
    const int outer = horizontal ? h : w;
    const int inner = horizontal ? w : h;
    for (int o = 0; o < outer; ++o) {
      for (int c = 0; c < 4; ++c) {
        double sum = 0.0;
        int hits = 0;
        /* 滑动窗口：先预热窗口再逐像素推进，避免 O(box * n)。 */
        for (int k = -box; k <= box; ++k) {
          if (k < 0 || k >= inner) continue;
          const size_t off = horizontal ? (static_cast<size_t>(o) * inner + k) * 4 + c
                                        : (static_cast<size_t>(k) * inner + o) * 4 + c;
          sum += src[off];
          ++hits;
        }
        for (int i = 0; i < inner; ++i) {
          const size_t off = horizontal ? (static_cast<size_t>(o) * inner + i) * 4 + c
                                        : (static_cast<size_t>(i) * inner + o) * 4 + c;
          dst[off] = static_cast<uint8_t>(std::lround(sum / std::max(1, hits)));
          const int add = i + box + 1;
          const int sub = i - box;
          if (add < inner) {
            const size_t o2 = horizontal ? (static_cast<size_t>(o) * inner + add) * 4 + c
                                         : (static_cast<size_t>(add) * inner + o) * 4 + c;
            sum += src[o2];
            ++hits;
          }
          if (sub >= 0) {
            const size_t o2 = horizontal ? (static_cast<size_t>(o) * inner + sub) * 4 + c
                                         : (static_cast<size_t>(sub) * inner + o) * 4 + c;
            sum -= src[o2];
            --hits;
          }
        }
      }
    }
    std::swap(src, dst);
  }
  if (src != buf) std::memcpy(buf, src, count * 4);
}

/* ---------------------------------------------------------- 各 kind 的实现 */

/* kind 9 — Invert。RGB 反色，alpha 保持（对齐上游 .invert）。 */
bool apply_invert(uint8_t* buf, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t* px = buf + i * 4;
    if (px[3] == 0) continue;
    px[0] = static_cast<uint8_t>(255 - px[0]);
    px[1] = static_cast<uint8_t>(255 - px[1]);
    px[2] = static_cast<uint8_t>(255 - px[2]);
  }
  return true;
}

/*
 * kind 0 — Hue/Saturation。
 * 走 3D cube（levels_apply 的 cube_apply 同款查表），与上游
 * `GPUAdjustment.cube(for:)`「把调整跑一遍整个颜色晶格」的思路一致：
 * 先按参数生成 N³ 的查找表，再逐像素查一次，避免逐像素重算 HSV。
 *
 * 预乘缓冲必须先反预乘：HSV 是在直通色上定义的，
 * 直接对预乘值做色相旋转会把「半透明白」和「纯白」混为一谈。
 */
constexpr int kHsvCubeDim = 33; /* 上游同量级：足够平滑又只占 1.4 MB */

/*
 * 按 Hue/Saturation 参数生成 dimension³ 的 RGBA float 查找表（红变化最快）。
 *
 * 抽出来是因为 8 位与 16 位两条路要用**同一张表**：8 位走 `cube_apply`，
 * 16 位走 `deep_cube_apply`，两者查的是同一组直通色映射。表各建一份，
 * 一边改了参数另一边就会静默地用旧语义——那正是本轮要消除的那类漂移。
 */
void build_hsv_cube(const Settings& s, int dim, std::vector<float>* cube) {
  const double hue = s.clamp(s.number("hue", nullptr, 0.0), -360.0, 360.0);
  const double sat = s.clamp(s.number("saturation", nullptr, 0.0), -100.0, 100.0);
  const double light = s.clamp(s.number("lightness", nullptr, 0.0), -100.0, 100.0);
  const bool colorize = s.boolean("colorize", nullptr, false);

  cube->assign(static_cast<size_t>(dim) * dim * dim * 4, 0.0f);
  const double step = 1.0 / (dim - 1);
  for (int r = 0; r < dim; ++r) {
    for (int g = 0; g < dim; ++g) {
      for (int b = 0; b < dim; ++b) {
        double rr = r * step, gg = g * step, bb = b * step;
        if (colorize) {
          /* Colorize：丢弃原色相，用固定色相 + 原饱和度/明度重新着色。 */
          const double maxc = std::max(rr, std::max(gg, bb));
          const double minc = std::min(rr, std::min(gg, bb));
          const double chroma = maxc - minc;
          double h = 0.0;
          if (chroma > 1e-9) {
            if (maxc == rr) h = 60.0 * std::fmod((gg - bb) / chroma, 6.0);
            else if (maxc == gg) h = 60.0 * ((bb - rr) / chroma + 2.0);
            else h = 60.0 * ((rr - gg) / chroma + 4.0);
            if (h < 0.0) h += 360.0;
          }
          const double l = (maxc + minc) * 0.5;
          const double sv = chroma / (1.0 - std::fabs(2.0 * l - 1.0) + 1e-9);
          hsv_to_rgb(hue, sv, l, &rr, &gg, &bb);
        } else {
          hsv_to_rgb(hue, sat / 100.0, light / 100.0, &rr, &gg, &bb);
        }
        const size_t idx = ((static_cast<size_t>(r) * dim + g) * dim + b) * 4;
        (*cube)[idx + 0] = static_cast<float>(std::clamp(rr, 0.0, 1.0));
        (*cube)[idx + 1] = static_cast<float>(std::clamp(gg, 0.0, 1.0));
        (*cube)[idx + 2] = static_cast<float>(std::clamp(bb, 0.0, 1.0));
        (*cube)[idx + 3] = 1.0f;
      }
    }
  }
}

/* Hue/Saturation 的参数是否恒等（表一旦恒等，查表就是白跑一趟）。 */
bool hsv_settings_is_identity(const Settings& s) {
  const double hue = s.clamp(s.number("hue", nullptr, 0.0), -360.0, 360.0);
  const double sat = s.clamp(s.number("saturation", nullptr, 0.0), -100.0, 100.0);
  const double light = s.clamp(s.number("lightness", nullptr, 0.0), -100.0, 100.0);
  const bool colorize = s.boolean("colorize", nullptr, false);
  return hue == 0.0 && sat == 0.0 && light == 0.0 && !colorize;
}

bool apply_hue_saturation(uint8_t* buf, size_t count, const Settings& s) {
  if (hsv_settings_is_identity(s)) return false;

  const int dim = kHsvCubeDim;
  std::vector<float> cube;
  build_hsv_cube(s, dim, &cube);
  /* cube_apply 内部自行反预乘/再预乘，此处不得再动 alpha。 */
  cube_apply(buf, count, cube.data(), dim);
  return true;
}

/*
 * kind 1 — Levels。
 * 查找表按上游 `LevelsFilter.run` 的公式逐字构建：
 *   apply(v) = (outputBlack + pow(clamp((v*255-black)/(white-black)), 1/gamma)
 *                        * (outputWhite - outputBlack)) / 255
 * 三条通道各自先过单通道 range，再过 RGB 合成 range（上游 Settings.apply）。
 */
struct LevelRange {
  double black = 0.0, white = 255.0, gamma = 1.0, outputBlack = 0.0, outputWhite = 255.0;

  void normalize() {
    if (!(white > black)) white = black + 1.0;
    if (!(gamma > 0.0)) gamma = 1.0;
    outputBlack = std::clamp(outputBlack, 0.0, 255.0);
    outputWhite = std::clamp(outputWhite, outputBlack, 255.0);
  }
  double apply(double value) const {
    const double s = std::clamp((value * 255.0 - black) / (white - black), 0.0, 1.0);
    return (outputBlack + std::pow(s, 1.0 / gamma) * (outputWhite - outputBlack)) / 255.0;
  }
};

LevelRange read_range(const JsonValue* ranges, int index) {
  LevelRange r;
  if (ranges == nullptr || !ranges->is_array()) return r;
  const JsonValue::Array& arr = ranges->as_array();
  if (index < 0 || static_cast<size_t>(index) >= arr.size()) return r;
  const JsonValue* entry = &arr[static_cast<size_t>(index)];
  if (entry == nullptr || !entry->is_object()) return r;
  r.black = entry->number_or("black", r.black);
  r.white = entry->number_or("white", r.white);
  r.gamma = entry->number_or("gamma", r.gamma);
  r.outputBlack = entry->number_or("outputBlack", r.outputBlack);
  r.outputWhite = entry->number_or("outputWhite", r.outputWhite);
  r.normalize();
  return r;
}

bool apply_levels(uint8_t* buf, size_t count, const Settings& s) {
  const JsonValue* levels = s.root.find("levels");
  const LevelRange rgb = read_range(levels == nullptr ? nullptr : levels->find("ranges"), 0);
  const LevelRange red = read_range(levels == nullptr ? nullptr : levels->find("ranges"), 1);
  const LevelRange green = read_range(levels == nullptr ? nullptr : levels->find("ranges"), 2);
  const LevelRange blue = read_range(levels == nullptr ? nullptr : levels->find("ranges"), 3);

  const LevelRange identity;
  const bool is_identity = rgb.black == identity.black && rgb.white == identity.white &&
                           rgb.gamma == identity.gamma && rgb.outputBlack == identity.outputBlack &&
                           rgb.outputWhite == identity.outputWhite &&
                           red.black == identity.black && red.white == identity.white &&
                           green.black == identity.black && green.white == identity.white &&
                           blue.black == identity.black && blue.white == identity.white;
  if (is_identity) return false;

  std::vector<float> tables(256 * 3);
  for (int c = 0; c < 3; ++c) {
    const LevelRange& own = (c == 0) ? red : (c == 1) ? green : blue;
    for (int i = 0; i < 256; ++i) {
      /* 上游：单通道 range 先作用，再过 RGB 合成 range。 */
      tables[c * 256 + i] = static_cast<float>(rgb.apply(own.apply(i / 255.0)));
    }
  }
  levels_apply(buf, count, tables.data());
  return true;
}

/*
 * kind 2 — Curves。
 * 逐通道控制点 + 单条 RGB 合成曲线；用单调分段线性插值建 256 项查找表。
 * 上游 CurvePoint 是 (x, y) 且允许非单调，本实现按折线逐段线性走，
 * 与上游 `LayerAdjustment.curves` 的求值口径一致（控制点之间线性）。
 */
struct Curve {
  std::vector<std::pair<double, double>> points; /* 已按 x 升序 */
  bool empty() const { return points.size() < 2; }

  double apply(double v) const {
    if (empty()) return v;
    if (v <= points.front().first) return points.front().second;
    if (v >= points.back().first) return points.back().second;
    for (size_t i = 1; i < points.size(); ++i) {
      const auto& a = points[i - 1];
      const auto& b = points[i];
      if (v <= b.first) {
        const double span = b.first - a.first;
        if (span <= 1e-9) return b.second;
        const double t = (v - a.first) / span;
        return a.second + t * (b.second - a.second);
      }
    }
    return points.back().second;
  }
};

Curve read_curve(const JsonValue* channels, int index) {
  Curve curve;
  if (channels == nullptr || !channels->is_array()) return curve;
  const JsonValue::Array& arr = channels->as_array();
  if (index < 0 || static_cast<size_t>(index) >= arr.size()) return curve;
  const JsonValue* entry = &arr[static_cast<size_t>(index)];
  if (entry == nullptr || !entry->is_array()) return curve;
  for (const JsonValue& point : entry->as_array()) {
    /*
     * 上游 `CurvePoint` 是 Codable struct，落到 JSON 是对象 {"x":N,"y":N}；
     * 同时接受紧凑的 [x, y] 数组形式（.comp 互通里两种都出现过）。
     */
    if (point.is_object()) {
      const JsonValue* x = point.find("x");
      const JsonValue* y = point.find("y");
      if (x == nullptr || y == nullptr || !x->is_number() || !y->is_number()) continue;
      curve.points.emplace_back(x->as_number(0.0), y->as_number(0.0));
      continue;
    }
    if (!point.is_array()) continue;
    const JsonValue::Array& pair = point.as_array();
    if (pair.size() < 2 || !pair[0].is_number() || !pair[1].is_number()) continue;
    curve.points.emplace_back(pair[0].as_number(0.0), pair[1].as_number(0.0));
  }
  std::sort(curve.points.begin(), curve.points.end());
  return curve;
}

bool apply_curves(uint8_t* buf, size_t count, const Settings& s) {
  const JsonValue* curves = s.root.find("curves");
  const JsonValue* channels = (curves == nullptr) ? nullptr : curves->find("channels");
  /* 上游 channels[0] 为 RGB 合成曲线，1/2/3 分别为 R/G/B。 */
  const Curve rgb = read_curve(channels, 0);
  const Curve red = read_curve(channels, 1);
  const Curve green = read_curve(channels, 2);
  const Curve blue = read_curve(channels, 3);
  if (rgb.empty() && red.empty() && green.empty() && blue.empty()) return false;

  /* 曲线是逐通道 256 项查找表，走 levels_apply（其内部自行反预乘/再预乘）。 */
  std::vector<float> tables(256 * 3);
  for (int c = 0; c < 3; ++c) {
    const Curve& own = (c == 0) ? red : (c == 1) ? green : blue;
    for (int i = 0; i < 256; ++i) {
      const double v = i / 255.0;
      /* 上游：单通道曲线覆盖 RGB 合成曲线（ch0 始终先过）。 */
      const double mapped = own.empty() ? rgb.apply(v) : own.apply(v);
      tables[c * 256 + i] = static_cast<float>(std::clamp(mapped, 0.0, 1.0));
    }
  }
  levels_apply(buf, count, tables.data());
  return true;
}

/* kind 3 — Exposure：2^EV 增益 + offset + gamma，作用于线性光。 */
bool apply_exposure(uint8_t* buf, size_t count, const Settings& s) {
  const double ev = s.clamp(s.number("exposureSettings", "exposure", 0.0), -20.0, 20.0);
  const double offset = s.clamp(s.number("exposureSettings", "offset", 0.0), -0.5, 0.5);
  const double gamma = s.clamp(s.number("exposureSettings", "gamma", 1.0), 0.01, 9.99);
  if (ev == 0.0 && offset == 0.0 && gamma == 1.0) return false;

  unpremultiply_inplace(buf, count);
  const double gain = std::pow(2.0, ev);
  const double inv_gamma = 1.0 / gamma;
  for (size_t i = 0; i < count; ++i) {
    uint8_t* px = buf + i * 4;
    if (px[3] == 0) continue;
    for (int c = 0; c < 3; ++c) {
      /* 线性化 -> 曝光 -> 反 gamma -> 回 sRGB -> 重新预乘。 */
      const double srgb = px[c] / 255.0;
      const double linear = (srgb <= 0.04045) ? srgb / 12.92 : std::pow((srgb + 0.055) / 1.055, 2.4);
      double v = linear * gain + offset;
      if (v < 0.0) v = 0.0;
      v = std::pow(v, inv_gamma);
      const double back = (v <= 0.0031308) ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
      px[c] = static_cast<uint8_t>(std::lround(std::clamp(back, 0.0, 1.0) * 255.0));
    }
  }
  rgba_clamp_premultiplied(buf, count);
  return true;
}

/* kind 4 — Gradient Map：亮度取表，RGB 换成表色。 */
bool apply_gradient_map(uint8_t* buf, int32_t width, int32_t height, size_t stride,
                        const Settings& s) {
  const JsonValue* node = s.root.find("gradientMapSettings");
  const auto chan = [&](const char* group, const char* key, double fallback) -> double {
    if (node == nullptr) return fallback;
    const JsonValue* g = node->find(group);
    if (g == nullptr) return fallback;
    return s.clamp(g->number_or(key, fallback), 0.0, 1.0);
  };
  const double sr = chan("shadows", "red", 0.0), sg = chan("shadows", "green", 0.0), sb = chan("shadows", "blue", 0.0);
  const double hr = chan("highlights", "red", 1.0), hg = chan("highlights", "green", 1.0), hb = chan("highlights", "blue", 1.0);
  bool reversed = false;
  if (node != nullptr) reversed = node->bool_or("reversed", false);

  uint8_t table[256 * 3];
  for (int i = 0; i < 256; ++i) {
    const double t = (reversed ? (255 - i) : i) / 255.0;
    table[i * 3 + 0] = static_cast<uint8_t>(std::lround((sr + (hr - sr) * t) * 255.0));
    table[i * 3 + 1] = static_cast<uint8_t>(std::lround((sg + (hg - sg) * t) * 255.0));
    table[i * 3 + 2] = static_cast<uint8_t>(std::lround((sb + (hb - sb) * t) * 255.0));
  }
  /* adjust_gradient_map 内部自行反预乘/再预乘（见 AdjustPixels.c）。 */
  adjust_gradient_map(buf, static_cast<size_t>(width), static_cast<size_t>(height), stride, table);
  return true;
}

/* kind 5 — Grain。origin/unitsPerPixel 固定为文档像素单位，与上游 mapping 的 scale=1 等价。 */
bool apply_grain(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const JsonValue* node = s.root.find("grainSettings");
  const auto num = [&](const char* key, double fallback) -> double {
    return node == nullptr ? fallback : node->number_or(key, fallback);
  };
  const double amount = std::clamp(num("amount", 25.0), 0.0, 100.0);
  const double size = std::clamp(num("size", 1.5), 0.5, 20.0);
  const double roughness = std::clamp(num("roughness", 50.0), 0.0, 100.0);
  const uint32_t seed = static_cast<uint32_t>(num("seed", 0.0));
  if (amount == 0.0) return false;
  adjust_grain(buf, static_cast<size_t>(width), static_cast<size_t>(height), stride, amount, size,
               roughness, seed, 0.0, 0.0, 1.0);
  return true;
}

/* kind 6 — Add Noise（参数为顶层键，见 adjustment_table 的 kAddNoiseParams）。 */
bool apply_add_noise(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const float amount = static_cast<float>(s.clamp(s.number("noiseAmount", nullptr, 10.0), 0.1, 400.0));
  const int gaussian = s.boolean("noiseGaussian", nullptr, false) ? 1 : 0;
  const int monochromatic = s.boolean("noiseMonochromatic", nullptr, false) ? 1 : 0;
  const uint32_t seed = static_cast<uint32_t>(s.number("noiseSeed", nullptr, 0.0));
  noise_add(buf, static_cast<size_t>(width), static_cast<size_t>(height), stride, amount, gaussian,
            monochromatic, seed);
  return true;
}

/*
 * kind 7 — Gaussian Blur。
 * 3 次盒式模糊逼近高斯（半径 sigma = radius * 0.5 * 3，与上游 LevelsFilter 同量级）。
 * 分离式：先水平后垂直，半径大时分块以避免临时缓冲过大。
 */
bool apply_gaussian_blur(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const double radius = s.clamp(s.number("blurRadius", nullptr, 10.0), 0.1, 250.0);
  if (radius <= 0.1) return false;

  const int passes = 3;
  const double sigma = radius * 0.5;
  const int box = std::max(1, static_cast<int>(std::lround(sigma * std::sqrt(12.0 / passes + 1.0))));
  box_blur_in_place(buf, width, height, stride, box);
  return true;
}

/* kind 8 — Motion Blur：沿 motionAngle 方向的线性斜坡平均（参数为顶层键）。 */
bool apply_motion_blur(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const double angle = s.clamp(s.number("motionAngle", nullptr, 0.0), -90.0, 90.0);
  const double distance = s.clamp(s.number("motionDistance", nullptr, 10.0), 1.0, 2000.0);
  if (distance <= 1.0) return false;

  const int steps = std::min(64, std::max(2, static_cast<int>(std::lround(distance / 2.0))));
  const double rad = angle * 3.14159265358979323846 / 180.0;
  const double dx = std::cos(rad) * distance / (steps - 1);
  const double dy = std::sin(rad) * distance / (steps - 1);

  const int w = static_cast<int>(width);
  const int h = static_cast<int>(height);
  std::vector<uint8_t> acc(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
  for (int s = 0; s < steps; ++s) {
    const double ox = dx * s;
    const double oy = dy * s;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const int sx = static_cast<int>(std::lround(x - ox));
        const int sy = static_cast<int>(std::lround(y - oy));
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
        const uint8_t* p = buf + (static_cast<size_t>(sy) * w + sx) * 4;
        uint8_t* a = acc.data() + (static_cast<size_t>(y) * w + x) * 4;
        for (int c = 0; c < 4; ++c) a[c] = static_cast<uint8_t>(std::min(255, a[c] + p[c]));
      }
    }
  }
  for (size_t i = 0; i < acc.size(); ++i) {
    buf[i] = static_cast<uint8_t>(acc[i] / steps);
  }
  return true;
}

/* kind 10 — Black & White。 */
bool apply_black_white(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const JsonValue* node = s.root.find("blackWhiteSettings");
  const auto num = [&](const char* key, double fallback) -> double {
    return node == nullptr ? fallback : node->number_or(key, fallback);
  };
  const float weights[6] = {
      static_cast<float>(num("reds", 40.0) / 100.0),
      static_cast<float>(num("yellows", 60.0) / 100.0),
      static_cast<float>(num("greens", 40.0) / 100.0),
      static_cast<float>(num("cyans", 40.0) / 100.0),
      static_cast<float>(num("blues", 20.0) / 100.0),
      static_cast<float>(num("magentas", 80.0) / 100.0),
  };
  const bool tint = node != nullptr && node->bool_or("tint", false);
  const double tintHue = node == nullptr ? 40.0 : node->number_or("tintHue", 40.0);
  const double tintSat = node == nullptr ? 20.0 : node->number_or("tintSaturation", 20.0);
  /* 全默认即为恒等（上游 40/60/40/40/20/80 即默认权重）。 */
  const bool is_identity = weights[0] == 0.4f && weights[1] == 0.6f && weights[2] == 0.4f &&
                           weights[3] == 0.4f && weights[4] == 0.2f && weights[5] == 0.8f && !tint;
  if (is_identity) return false;

  adjust_black_white(buf, static_cast<size_t>(width), static_cast<size_t>(height), stride, weights,
                      tint ? 1 : 0, tintHue, tintSat / 100.0);
  return true;
}

/* kind 11 — Color Balance。 */
bool apply_color_balance(uint8_t* buf, int32_t width, int32_t height, size_t stride, const Settings& s) {
  const JsonValue* node = s.root.find("colorBalanceSettings");
  /* 三个色调区 × 三个通道，键名与 adjustment_table 的 kColorBalanceParams 一一对应。 */
  const auto range = [&](const char* prefix, float out[3]) {
    if (node == nullptr) return;
    char key[64];
    std::snprintf(key, sizeof(key), "%sCyanRed", prefix);
    out[0] = static_cast<float>(node->number_or(key, 0.0));
    std::snprintf(key, sizeof(key), "%sMagentaGreen", prefix);
    out[1] = static_cast<float>(node->number_or(key, 0.0));
    std::snprintf(key, sizeof(key), "%sYellowBlue", prefix);
    out[2] = static_cast<float>(node->number_or(key, 0.0));
  };
  float shadows[3] = {0.0f, 0.0f, 0.0f};
  float midtones[3] = {0.0f, 0.0f, 0.0f};
  float highlights[3] = {0.0f, 0.0f, 0.0f};
  range("shadow", shadows);
  range("mid", midtones);
  range("highlight", highlights);
  for (float* trip_ptr : {shadows, midtones, highlights}) {
    for (int i = 0; i < 3; ++i) trip_ptr[i] = std::clamp(trip_ptr[i] / 100.0f, -1.0f, 1.0f);
  }
  const bool preserve = node == nullptr || node->bool_or("preserveLuminosity", true);
  const bool is_identity = shadows[0] == 0.0f && shadows[1] == 0.0f && shadows[2] == 0.0f &&
                           midtones[0] == 0.0f && midtones[1] == 0.0f && midtones[2] == 0.0f &&
                           highlights[0] == 0.0f && highlights[1] == 0.0f && highlights[2] == 0.0f;
  if (is_identity) return false;

  adjust_color_balance(buf, static_cast<size_t>(width), static_cast<size_t>(height), stride, shadows,
                       midtones, highlights, preserve ? 1 : 0);
  return true;
}

}  // namespace

/*
 * 把像素模糊成一份**副本**（不改原图），sigma 以像素计。
 *
 * 与 kind 7 共用上面的 box_blur_in_place：扫描线的磷光晕（Scanlines.swift
 * glowing）要的正是「线本身的模糊副本再叠加回原图」，上游用 Core Image 的
 * 高斯模糊，本仓统一用这份三趟盒式近似。两处各抄一份模糊实现，参数一改就会
 * 漂移，所以这里只做搬运，不做第二份算法。
 *
 * 本函数在匿名命名空间**之外**（它要供 tool_ops 调用），但仍复用内部的
 * box_blur_in_place——后者保持内部链接，不外泄。
 */
bool blur_rgba8_copy(const uint8_t* src, int32_t width, int32_t height, size_t stride,
                     float sigma_px, std::vector<uint8_t>* out) {
  if (out == nullptr) return false;
  out->clear();
  if (src == nullptr || width <= 0 || height <= 0) return false;
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  if (stride < row_bytes) return false;
  if (!std::isfinite(sigma_px) || sigma_px <= 0.0f) {
    /* sigma <= 0 就是「不模糊」。按上游 glowing 的 sigma = spacing*3+3 恒 > 0，
     * 这里把退化情形显式拒绝，而不是悄悄返回一份未模糊的副本。 */
    return false;
  }
  const double sigma = static_cast<double>(sigma_px) * 0.5;
  const int box = std::max(1, static_cast<int>(std::lround(sigma * std::sqrt(12.0 / 3.0 + 1.0))));
  out->assign(row_bytes * static_cast<size_t>(height), 0);
  for (int32_t y = 0; y < height; ++y) {
    std::memcpy(out->data() + static_cast<size_t>(y) * row_bytes,
                src + static_cast<size_t>(y) * stride, row_bytes);
  }
  box_blur_in_place(out->data(), width, height, row_bytes, box);
  return true;
}

bool adjustment_kind_is_evaluated(uint32_t kind) { return kind < kAdjustmentKindCount; }

/*
 * 在 **16 位**像素上施加会混合通道的调整（当前只有 Hue/Saturation）。
 *
 * 为什么需要：上游 `DeepLayer.lookup` 把原因写得很明白——`run`（四趟
 * split/merge）读的是「每个通道自己那条线」，跨通道的调整（Hue/Sat、黑白、
 * 色彩平衡、渐变映射）不跟着本通道走，硬走四趟出色带，所以走 3D cube 查表。
 * 本仓此前只有 8 位的 `cube_apply`，16 位图层上的色相调整只能先降位再调，
 * 丢掉 16 位精度——正是 AUDIT8 §六 记的那条缺口。
 *
 * 口径：与 8 位那条路**共用同一张查找表**（build_hsv_cube），只换内核为
 * `deep_cube_apply`。恒等参数返回 false（不查表、白跑一趟）。
 */
bool apply_channel_mixing_sixteen_bit(uint16_t* pixels, size_t stride, int32_t width, int32_t height,
                                      const std::string& settings_json) {
  if (pixels == nullptr || width <= 0 || height <= 0) return false;
  Settings s;
  s.load(settings_json);
  if (hsv_settings_is_identity(s)) return false;

  const int dim = 33;   /* 与 8 位同（kHsvCubeDim） */
  std::vector<float> cube;
  build_hsv_cube(s, dim, &cube);
  deep_cube_apply(pixels, stride, static_cast<size_t>(width), static_cast<size_t>(height),
                  cube.data(), dim);
  return true;
}

bool apply_adjustments(const std::vector<Adjustment>& adjustments, uint8_t* buf, int32_t width,
                       int32_t height, size_t stride, AdjustmentEvalResult* result,
                       std::string* error) {
  if (result != nullptr) *result = AdjustmentEvalResult{};
  if (adjustments.empty()) return true;
  if (buf == nullptr || width <= 0 || height <= 0) {
    if (error != nullptr) *error = "adjustment target buffer is invalid";
    return false;
  }
  const size_t count = pixel_count(width, height, stride);
  if (count == 0) return true;

  for (const Adjustment& adjustment : adjustments) {
    if (!adjustment_kind_is_evaluated(adjustment.kind)) {
      if (result != nullptr) ++result->skipped;
      continue;
    }
    Settings settings;
    settings.load(adjustment.settings_json);

    bool changed = false;
    switch (adjustment.kind) {
      case 0: changed = apply_hue_saturation(buf, count, settings); break;
      case 1: changed = apply_levels(buf, count, settings); break;
      case 2: changed = apply_curves(buf, count, settings); break;
      case 3: changed = apply_exposure(buf, count, settings); break;
      case 4: changed = apply_gradient_map(buf, width, height, stride, settings); break;
      case 5: changed = apply_grain(buf, width, height, stride, settings); break;
      case 6: changed = apply_add_noise(buf, width, height, stride, settings); break;
      case 7: changed = apply_gaussian_blur(buf, width, height, stride, settings); break;
      case 8: changed = apply_motion_blur(buf, width, height, stride, settings); break;
      case 9: changed = apply_invert(buf, count); break;
      case 10: changed = apply_black_white(buf, width, height, stride, settings); break;
      case 11: changed = apply_color_balance(buf, width, height, stride, settings); break;
      default: break;
    }
    if (result != nullptr) {
      ++result->applied;
      if (changed) result->changed = true;
    }
  }
  return true;
}

}  // namespace compositor

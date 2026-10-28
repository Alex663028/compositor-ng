// AI生成
#include "compositing/blend.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

/* ---------------------------------------------------- 分离型单通道函数 */

double f_darken(double cb, double cs) { return std::min(cb, cs); }
double f_lighten(double cb, double cs) { return std::max(cb, cs); }
double f_multiply(double cb, double cs) { return cb * cs; }
double f_screen(double cb, double cs) { return cb + cs - cb * cs; }

double f_color_dodge(double cb, double cs) {
  if (cs >= 1.0) return 1.0;
  return clamp01(cb / (1.0 - cs));
}

double f_color_burn(double cb, double cs) {
  if (cs <= 0.0) return 0.0;
  return clamp01(1.0 - (1.0 - cb) / cs);
}

double f_linear_burn(double cb, double cs) { return clamp01(cb + cs - 1.0); }
double f_linear_dodge(double cb, double cs) { return clamp01(cb + cs); }

double f_hard_light(double cb, double cs) {
  if (cs <= 0.5) return clamp01(2.0 * cb * cs);
  return clamp01(1.0 - 2.0 * (1.0 - cb) * (1.0 - cs));
}

double f_overlay(double cb, double cs) {
  /* overlay(cb, cs) == hard_light(cs, cb) */
  return f_hard_light(cs, cb);
}

double f_soft_light(double cb, double cs) {
  if (cs <= 0.5) {
    return clamp01(cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb));
  }
  const double d = (cb <= 0.25) ? (((16.0 * cb - 12.0) * cb + 4.0) * cb)
                                : std::sqrt(cb);
  return clamp01(cb + (2.0 * cs - 1.0) * (d - cb));
}

double f_vivid_light(double cb, double cs) {
  if (cs <= 0.5) return f_color_burn(cb, clamp01(2.0 * cs));
  return f_color_dodge(cb, clamp01(2.0 * cs - 1.0));
}

double f_linear_light(double cb, double cs) { return clamp01(cb + 2.0 * cs - 1.0); }

double f_pin_light(double cb, double cs) {
  if (cs <= 0.5) return std::min(cb, clamp01(2.0 * cs));
  return std::max(cb, clamp01(2.0 * cs - 1.0));
}

double f_hard_mix(double cb, double cs) {
  /* 规范定义：以 Vivid Light 结果为阈值，二值化。 */
  return f_vivid_light(cb, cs) < 0.5 ? 0.0 : 1.0;
}

double f_difference(double cb, double cs) { return std::fabs(cb - cs); }
double f_exclusion(double cb, double cs) { return clamp01(cb + cs - 2.0 * cb * cs); }
double f_subtract(double cb, double cs) { return clamp01(cb - cs); }
double f_divide(double cb, double cs) {
  if (cs <= 0.0) return 0.0;
  return clamp01(cb / cs);
}

/* -------------------------------------------------- 非分离型辅助函数 */

double lum(const double c[3]) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }

void clip_color(double c[3]) {
  const double l = lum(c);
  const double n = std::min(c[0], std::min(c[1], c[2]));
  const double x = std::max(c[0], std::max(c[1], c[2]));
  if (n < 0.0) {
    for (int i = 0; i < 3; ++i) c[i] = l + ((c[i] - l) * l) / (l - n);
  }
  if (x > 1.0) {
    for (int i = 0; i < 3; ++i) c[i] = l + ((c[i] - l) * (1.0 - l)) / (x - l);
  }
}

void set_lum(double c[3], double l) {
  const double d = l - lum(c);
  c[0] += d;
  c[1] += d;
  c[2] += d;
  clip_color(c);
}

double sat(const double c[3]) {
  return std::max(c[0], std::max(c[1], c[2])) - std::min(c[0], std::min(c[1], c[2]));
}

void set_sat(double c[3], double s) {
  /* 规范算法：对 (mid, min, max) 三元组按目标饱和度重排。 */
  int idx_min = 0;
  int idx_mid = 1;
  int idx_max = 2;
  if (c[idx_min] > c[idx_mid]) std::swap(idx_min, idx_mid);
  if (c[idx_mid] > c[idx_max]) std::swap(idx_mid, idx_max);
  if (c[idx_min] > c[idx_mid]) std::swap(idx_min, idx_mid);

  if (c[idx_max] > c[idx_min]) {
    c[idx_mid] = ((c[idx_mid] - c[idx_min]) * s) / (c[idx_max] - c[idx_min]);
    c[idx_max] = s;
  } else {
    c[idx_mid] = 0.0;
    c[idx_max] = 0.0;
  }
  c[idx_min] = 0.0;
}

/* -------------------------------- .comp 规范字符串（上游 LayerBlendMode） */

/*
 * 顺序即 comp_blend_mode 的数值顺序，也即上游 LayerBlendMode 的声明顺序
 * （Photoshop 分组：正常 / 变暗组 / 变亮组 / 对比组 / 差值组 / 分量组）。
 * 测试 test_compositing.cpp 会逐字比对这张表，防止 ABI 冻结后漂移。
 */
struct BlendName {
  comp_blend_mode mode;
  const char* name;
};

const BlendName kBlendNames[] = {
    {COMP_BLEND_NORMAL, "Normal"},
    {COMP_BLEND_DARKEN, "Darken"},
    {COMP_BLEND_MULTIPLY, "Multiply"},
    {COMP_BLEND_COLOR_BURN, "Color Burn"},
    {COMP_BLEND_LINEAR_BURN, "Linear Burn"},
    {COMP_BLEND_LIGHTEN, "Lighten"},
    {COMP_BLEND_SCREEN, "Screen"},
    {COMP_BLEND_COLOR_DODGE, "Color Dodge"},
    {COMP_BLEND_LINEAR_DODGE, "Linear Dodge (Add)"},
    {COMP_BLEND_OVERLAY, "Overlay"},
    {COMP_BLEND_SOFT_LIGHT, "Soft Light"},
    {COMP_BLEND_HARD_LIGHT, "Hard Light"},
    {COMP_BLEND_VIVID_LIGHT, "Vivid Light"},
    {COMP_BLEND_LINEAR_LIGHT, "Linear Light"},
    {COMP_BLEND_PIN_LIGHT, "Pin Light"},
    {COMP_BLEND_HARD_MIX, "Hard Mix"},
    {COMP_BLEND_DIFFERENCE, "Difference"},
    {COMP_BLEND_EXCLUSION, "Exclusion"},
    {COMP_BLEND_SUBTRACT, "Subtract"},
    {COMP_BLEND_DIVIDE, "Divide"},
    {COMP_BLEND_HUE, "Hue"},
    {COMP_BLEND_SATURATION, "Saturation"},
    {COMP_BLEND_COLOR, "Color"},
    {COMP_BLEND_LUMINOSITY, "Luminosity"},
    /* A50：本仓扩展（超出上游 24）。名字取 PS 拼音规范名；上游无对应 rawValue。 */
    {COMP_BLEND_DISSOLVE, "Dissolve"},
    {COMP_BLEND_DARKER_COLOR, "Darker Color"},
    {COMP_BLEND_LIGHTER_COLOR, "Lighter Color"},
};

constexpr size_t kBlendNameCount = sizeof(kBlendNames) / sizeof(kBlendNames[0]);

}  // namespace

const char* blend_mode_manifest_name(comp_blend_mode mode) {
  for (size_t i = 0; i < kBlendNameCount; ++i) {
    if (kBlendNames[i].mode == mode) return kBlendNames[i].name;
  }
  return nullptr;
}

bool blend_mode_from_manifest_name(const char* name, size_t length, comp_blend_mode* out) {
  if (name == nullptr || out == nullptr || length == 0) return false;
  for (size_t i = 0; i < kBlendNameCount; ++i) {
    const char* candidate = kBlendNames[i].name;
    if (std::strlen(candidate) == length && std::memcmp(candidate, name, length) == 0) {
      *out = kBlendNames[i].mode;
      return true;
    }
  }
  return false;
}

double blend_separable(comp_blend_mode mode, double cb, double cs) {
  switch (mode) {
    case COMP_BLEND_NORMAL:       return cs;
    case COMP_BLEND_DARKEN:       return f_darken(cb, cs);
    case COMP_BLEND_MULTIPLY:     return f_multiply(cb, cs);
    case COMP_BLEND_COLOR_BURN:   return f_color_burn(cb, cs);
    case COMP_BLEND_LINEAR_BURN:  return f_linear_burn(cb, cs);
    case COMP_BLEND_LIGHTEN:      return f_lighten(cb, cs);
    case COMP_BLEND_SCREEN:       return f_screen(cb, cs);
    case COMP_BLEND_COLOR_DODGE:  return f_color_dodge(cb, cs);
    case COMP_BLEND_LINEAR_DODGE: return f_linear_dodge(cb, cs);
    case COMP_BLEND_OVERLAY:      return f_overlay(cb, cs);
    case COMP_BLEND_SOFT_LIGHT:   return f_soft_light(cb, cs);
    case COMP_BLEND_HARD_LIGHT:   return f_hard_light(cb, cs);
    case COMP_BLEND_VIVID_LIGHT:  return f_vivid_light(cb, cs);
    case COMP_BLEND_LINEAR_LIGHT: return f_linear_light(cb, cs);
    case COMP_BLEND_PIN_LIGHT:    return f_pin_light(cb, cs);
    case COMP_BLEND_HARD_MIX:     return f_hard_mix(cb, cs);
    case COMP_BLEND_DIFFERENCE:   return f_difference(cb, cs);
    case COMP_BLEND_EXCLUSION:    return f_exclusion(cb, cs);
    case COMP_BLEND_SUBTRACT:     return f_subtract(cb, cs);
    case COMP_BLEND_DIVIDE:       return f_divide(cb, cs);
    /* 非分离型不走此路径，退回 Normal 以免静默产生错误结果。 */
    default:                      return cs;
  }
}

void blend_rgb(comp_blend_mode mode, const double base[3], const double src[3],
               double out[3]) {
  switch (mode) {
    case COMP_BLEND_HUE: {
      double t[3] = {src[0], src[1], src[2]};
      set_sat(t, sat(base));
      set_lum(t, lum(base));
      out[0] = t[0];
      out[1] = t[1];
      out[2] = t[2];
      return;
    }
    case COMP_BLEND_SATURATION: {
      double t[3] = {base[0], base[1], base[2]};
      set_sat(t, sat(src));
      set_lum(t, lum(base));
      out[0] = t[0];
      out[1] = t[1];
      out[2] = t[2];
      return;
    }
    case COMP_BLEND_COLOR: {
      double t[3] = {src[0], src[1], src[2]};
      set_lum(t, lum(base));
      out[0] = t[0];
      out[1] = t[1];
      out[2] = t[2];
      return;
    }
    case COMP_BLEND_LUMINOSITY: {
      double t[3] = {base[0], base[1], base[2]};
      set_lum(t, lum(src));
      out[0] = t[0];
      out[1] = t[1];
      out[2] = t[2];
      return;
    }
    /*
     * A50：本仓超出上游的两种。Darker Color / Lighter Color 是**整像素**亮度择色
     * （不是逐通道 min/max），走不了 blend_separable，只能在这里整像素挑一颗。
     * 与 PS / W3C 非分离型口径一致：按相对亮度（Rec.601 权重）比较，取整颗像素。
     */
    case COMP_BLEND_DARKER_COLOR: {
      const double* pick = (lum(base) <= lum(src)) ? base : src;
      out[0] = pick[0];
      out[1] = pick[1];
      out[2] = pick[2];
      return;
    }
    case COMP_BLEND_LIGHTER_COLOR: {
      const double* pick = (lum(base) >= lum(src)) ? base : src;
      out[0] = pick[0];
      out[1] = pick[1];
      out[2] = pick[2];
      return;
    }
    default: {
      out[0] = blend_separable(mode, base[0], src[0]);
      out[1] = blend_separable(mode, base[1], src[1]);
      out[2] = blend_separable(mode, base[2], src[2]);
      return;
    }
  }
}

bool is_blend_supported(comp_blend_mode mode) {
  /*
   * A50：上界抬到 LIGHTER_COLOR。三种扩展模式同样是需要真实求值的合法模式；
   * 若沿用旧上界（LUMINOSITY），它们会被判为「越界」而静默退回 Normal。
   */
  return mode >= COMP_BLEND_NORMAL && mode <= COMP_BLEND_LIGHTER_COLOR;
}

}  // namespace compositor

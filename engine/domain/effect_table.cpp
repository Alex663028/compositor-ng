// AI生成
/*
 * effect_table.cpp — 效果参数表的实现。
 *
 * 路径读取复用 `adjustment_read_value`：两者要的语义完全相同（按点分路径取标量、
 * 路径缺失/中间段非对象/叶子非标量时返回 false），效果只是键都落在顶层。
 * 共用一份实现可以避免「同一个 JSON 路径规则写两遍、改一处忘一处」。
 */
#include "domain/effect_table.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "domain/adjustment_table.h"
#include "format/comp_json.h"

namespace compositor {
namespace {

/* Stroke：描边宽度 / 不透明度 / 内侧标志 / 颜色。 */
const EffectParam kStrokeParams[] = {
    {"size", "Size (px)", kEffectParamNumber, 0.0, 500.0, 4.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"inside", "Inside (0/1)", kEffectParamBool, 0.0, 1.0, 0.0},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Drop Shadow：角度 / 距离 / 模糊 / 不透明度 / 颜色。 */
const EffectParam kDropShadowParams[] = {
    {"angle", "Angle (deg)", kEffectParamNumber, -360.0, 360.0, 90.0},
    {"distance", "Distance (px)", kEffectParamNumber, 0.0, 5000.0, 20.0},
    {"blur", "Blur (px)", kEffectParamNumber, 0.0, 500.0, 20.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.5},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Color Overlay：不透明度 / 颜色。 */
const EffectParam kColorOverlayParams[] = {
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Inner Shadow：角度 / 距离 / 模糊 / 不透明度 / 颜色。 */
const EffectParam kInnerShadowParams[] = {
    {"angle", "Angle (deg)", kEffectParamNumber, -360.0, 360.0, 90.0},
    {"distance", "Distance (px)", kEffectParamNumber, 0.0, 5000.0, 10.0},
    {"blur", "Blur (px)", kEffectParamNumber, 0.0, 500.0, 10.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.5},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Outer Glow：尺寸 / 不透明度 / 颜色。 */
const EffectParam kOuterGlowParams[] = {
    {"size", "Size (px)", kEffectParamNumber, 0.0, 500.0, 20.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.75},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Inner Glow：尺寸 / 不透明度 / 颜色。 */
const EffectParam kInnerGlowParams[] = {
    {"size", "Size (px)", kEffectParamNumber, 0.0, 500.0, 10.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.75},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Bevel & Emboss（P1-1）：角度 / 高度 / 深度 / 尺寸 / 柔化 / 不透明度 / 双色。 */
const EffectParam kBevelParams[] = {
    {"angle", "Angle (deg)", kEffectParamNumber, -360.0, 360.0, 120.0},
    {"altitude", "Altitude (deg)", kEffectParamNumber, 0.0, 90.0, 30.0},
    {"depth", "Depth (%)", kEffectParamNumber, 0.0, 1000.0, 100.0},
    {"size", "Size (px)", kEffectParamNumber, 0.0, 500.0, 10.0},
    {"soften", "Soften (px)", kEffectParamNumber, 0.0, 500.0, 0.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.75},
    {"red", "Highlight R", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"green", "Highlight G", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"blue", "Highlight B", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"shadow_red", "Shadow R", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"shadow_green", "Shadow G", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"shadow_blue", "Shadow B", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Satin（P1-1）：角度 / 距离 / 模糊 / 取反 / 不透明度 / 颜色。 */
const EffectParam kSatinParams[] = {
    {"angle", "Angle (deg)", kEffectParamNumber, -360.0, 360.0, 19.0},
    {"distance", "Distance (px)", kEffectParamNumber, 0.0, 5000.0, 11.0},
    {"blur", "Blur (px)", kEffectParamNumber, 0.0, 500.0, 14.0},
    {"invert", "Invert (0/1)", kEffectParamBool, 0.0, 1.0, 0.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 0.5},
    {"red", "Red (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Green (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Blue (0-1)", kEffectParamNumber, 0.0, 1.0, 0.0},
};

/* Gradient Overlay（P1-1）：角度 / 反向 / 不透明度 / 起点色 / 终点色。 */
const EffectParam kGradientOverlayParams[] = {
    {"angle", "Angle (deg)", kEffectParamNumber, -360.0, 360.0, 90.0},
    {"reverse", "Reverse (0/1)", kEffectParamBool, 0.0, 1.0, 0.0},
    {"opacity", "Opacity (0-1)", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"red", "Start R", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"green", "Start G", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"blue", "Start B", kEffectParamNumber, 0.0, 1.0, 0.0},
    {"red2", "End R", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"green2", "End G", kEffectParamNumber, 0.0, 1.0, 1.0},
    {"blue2", "End B", kEffectParamNumber, 0.0, 1.0, 1.0},
};

#define COMP_EFFECT_PARAMS(array) array, sizeof(array) / sizeof((array)[0])

/* 下标 = effect kind；顺序与 effect_eval.h 的枚举逐字对应。 */
const EffectKindInfo kKinds[kEffectParamKindCount] = {
    {0u, COMP_EFFECT_PARAMS(kStrokeParams)},
    {1u, COMP_EFFECT_PARAMS(kDropShadowParams)},
    {2u, COMP_EFFECT_PARAMS(kColorOverlayParams)},
    {3u, COMP_EFFECT_PARAMS(kInnerShadowParams)},
    {4u, COMP_EFFECT_PARAMS(kOuterGlowParams)},
    {5u, COMP_EFFECT_PARAMS(kInnerGlowParams)},
    {6u, COMP_EFFECT_PARAMS(kBevelParams)},
    {7u, COMP_EFFECT_PARAMS(kSatinParams)},
    {8u, COMP_EFFECT_PARAMS(kGradientOverlayParams)},
};

#undef COMP_EFFECT_PARAMS

bool parse_bool_text(const std::string& text, bool* out) {
  if (text == "1" || text == "true" || text == "TRUE" || text == "True") {
    *out = true;
    return true;
  }
  if (text == "0" || text == "false" || text == "FALSE" || text == "False") {
    *out = false;
    return true;
  }
  return false;
}

std::string number_to_text(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  return std::string(buffer);
}

}  // namespace

const EffectKindInfo* effect_kind_info(uint32_t effect) {
  if (effect >= kEffectParamKindCount) return nullptr;
  return &kKinds[effect];
}

bool effect_read_value(const std::string& settings_json, const char* json_path,
                       double* out_number, int32_t* out_boolean) {
  return adjustment_read_value(settings_json, json_path, out_number, out_boolean);
}

bool effect_build_settings(uint32_t effect, const std::vector<std::string>& values,
                           const std::string& base_json, std::string* out_json,
                           std::string* error) {
  const EffectKindInfo* info = effect_kind_info(effect);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown effect kind";
    return false;
  }
  if (values.size() != info->param_count) {
    if (error != nullptr) *error = "value count does not match parameter count";
    return false;
  }

  const std::string base = base_json.empty() ? std::string("{}") : base_json;
  std::string parse_error;
  JsonValue root = json_parse(base, &parse_error);
  if (!root.is_object()) {
    if (error != nullptr) {
      *error = parse_error.empty() ? std::string("settings must be a json object")
                                   : "invalid settings json: " + parse_error;
    }
    return false;
  }

  for (size_t i = 0; i < info->param_count; ++i) {
    const EffectParam& param = info->params[i];
    /* 空串 = 该项不动（沿用既有值；既有值是缺省时等价于引擎缺省）。 */
    if (values[i].empty()) continue;

    JsonValue leaf;
    if (param.type == kEffectParamBool) {
      bool truthy = false;
      if (!parse_bool_text(values[i], &truthy)) {
        if (error != nullptr) *error = std::string(param.json_path) + ": expected boolean text";
        return false;
      }
      leaf = JsonValue::boolean(truthy);
    } else {
      const char* begin = values[i].c_str();
      char* end = nullptr;
      const double number = std::strtod(begin, &end);
      if (end == begin || end == nullptr || *end != '\0' || !std::isfinite(number)) {
        if (error != nullptr) *error = std::string(param.json_path) + ": expected numeric text";
        return false;
      }
      if (number < param.min_value || number > param.max_value) {
        if (error != nullptr) {
          *error = std::string(param.json_path) + " out of range [" +
                   number_to_text(param.min_value) + ", " + number_to_text(param.max_value) + "]";
        }
        return false;
      }
      leaf = JsonValue::number(number);
    }
    /* 效果的参数键都是单段（扁平对象），直接覆盖顶层键。 */
    root.set(param.json_path, std::move(leaf));
  }

  if (out_json != nullptr) *out_json = json_dump(root, 0);
  return true;
}

}  // namespace compositor

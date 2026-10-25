// AI生成
/*
 * fill_layer.cpp — 填充层的参数表、校验与渲染（唯一实现）。
 *
 * 渲染口径（逐条固定，便于三端与测试按字节对齐）：
 *   - 输出一律预乘 RGBA8、行优先、原点在图层像素空间 (0,0)；
 *   - 纯色：color × alpha 预乘；
 *   - 渐变：两色标按「投影比例」插值，angle 从 +x 轴逆时针量（0° = 左→右，
 *     90° = 下→上，与 Photoshop 同向）；normalize 到画布包围盒的投影跨度，
 *     因此任何角度都铺满整幅；reverse 交换两端；
 *   - 图案：按 size 平铺的内置图案（0 棋盘 / 1 竖条 / 2 横条 / 3 圆点），
 *     全部是整数格点运算，无随机源；
 *   - 不含 dither：填充的用途是与上游「几何可复现」对齐，逐字节稳定优先。
 */
#include "domain/fill_layer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/comp_geometry.h"
#include "domain/settings_json_util.h"
#include "format/comp_json.h"

namespace compositor {
namespace {

#define COMP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

using settings_json::locate_leaf;
using settings_json::number_to_text;
using settings_json::parse_bool_text;
using settings_json::set_path;
using settings_json::split_path;

/* 1 纯色：RGBA 各 0..255（与 PS 的 8 位拾色器同量程），alpha 缺省不透明。 */
const FillParam kSolidParams[] = {
    {"color.red", "Red", kFillParamNumber, 0.0, 255.0, 128.0},
    {"color.green", "Green", kFillParamNumber, 0.0, 255.0, 128.0},
    {"color.blue", "Blue", kFillParamNumber, 0.0, 255.0, 128.0},
    {"color.alpha", "Alpha", kFillParamNumber, 0.0, 255.0, 255.0},
};

/* 2 渐变：两个色标 + 角度 + 反向。角度 360 与 0 等价（不折叠存储，便于面板回显）。 */
const FillParam kGradientParams[] = {
    {"start.red", "Start R", kFillParamNumber, 0.0, 255.0, 255.0},
    {"start.green", "Start G", kFillParamNumber, 0.0, 255.0, 0.0},
    {"start.blue", "Start B", kFillParamNumber, 0.0, 255.0, 0.0},
    {"start.alpha", "Start A", kFillParamNumber, 0.0, 255.0, 255.0},
    {"end.red", "End R", kFillParamNumber, 0.0, 255.0, 0.0},
    {"end.green", "End G", kFillParamNumber, 0.0, 255.0, 0.0},
    {"end.blue", "End B", kFillParamNumber, 0.0, 255.0, 255.0},
    {"end.alpha", "End A", kFillParamNumber, 0.0, 255.0, 255.0},
    {"angle", "Angle", kFillParamNumber, 0.0, 360.0, 0.0},
    {"reverse", "Reverse", kFillParamBool, 0.0, 1.0, 0.0},
};

/* 3 图案：内置图案序号 + 单元格边长 + 前景/背景两色。 */
const FillParam kPatternParams[] = {
    {"pattern", "Pattern", kFillParamNumber, 0.0, 3.0, 0.0},
    {"size", "Size", kFillParamNumber, 2.0, 256.0, 16.0},
    {"foreground.red", "Fore R", kFillParamNumber, 0.0, 255.0, 0.0},
    {"foreground.green", "Fore G", kFillParamNumber, 0.0, 255.0, 0.0},
    {"foreground.blue", "Fore B", kFillParamNumber, 0.0, 255.0, 0.0},
    {"foreground.alpha", "Fore A", kFillParamNumber, 0.0, 255.0, 255.0},
    {"background.red", "Back R", kFillParamNumber, 0.0, 255.0, 255.0},
    {"background.green", "Back G", kFillParamNumber, 0.0, 255.0, 255.0},
    {"background.blue", "Back B", kFillParamNumber, 0.0, 255.0, 255.0},
    {"background.alpha", "Back A", kFillParamNumber, 0.0, 255.0, 255.0},
};

/* 顺序即 kind 编号（0 不是填充层，故第一项为占位的 nullptr 语义）。 */
const FillKindInfo kKinds[kFillKindCount] = {
    {kFillKindNone, "None", "Not a fill layer", "Layer", nullptr, 0},
    {kFillKindSolid, "Solid Color", "Fill the layer with one color", "Color Fill", kSolidParams,
     COMP_ARRAY_LEN(kSolidParams)},
    {kFillKindGradient, "Gradient", "Linear two-color gradient across the canvas", "Gradient Fill",
     kGradientParams, COMP_ARRAY_LEN(kGradientParams)},
    {kFillKindPattern, "Pattern", "Tile a built-in pattern over the canvas", "Pattern Fill",
     kPatternParams, COMP_ARRAY_LEN(kPatternParams)},
};

/* 顶层键白名单 = 各参数路径的根段。填充表没有 extra_roots。 */
bool root_allowed(const FillKindInfo& info, const std::string& key) {
  for (size_t i = 0; i < info.param_count; ++i) {
    const char* path = info.params[i].json_path;
    const char* dot = std::strchr(path, '.');
    const size_t length = dot != nullptr ? static_cast<size_t>(dot - path) : std::strlen(path);
    if (key.size() == length && key.compare(0, length, path, length) == 0) return true;
  }
  return false;
}

JsonValue build_defaults(const FillKindInfo& info) {
  JsonValue root = JsonValue::object({});
  for (size_t i = 0; i < info.param_count; ++i) {
    const FillParam& param = info.params[i];
    JsonValue leaf = param.type == kFillParamBool
                         ? JsonValue::boolean(param.default_value != 0.0)
                         : JsonValue::number(param.default_value);
    root = set_path(root, split_path(param.json_path), 0, std::move(leaf));
  }
  return root;
}

/* 面板/渲染共用的取值：缺失或类型不符一律回落到参数表缺省值。 */
double value_or_default(const FillKindInfo& info, size_t index, const JsonValue& root) {
  const FillParam& param = info.params[index];
  bool malformed = false;
  const JsonValue* leaf = locate_leaf(root, param.json_path, &malformed);
  if (leaf == nullptr || malformed) return param.default_value;
  if (param.type == kFillParamBool) {
    return leaf->is_bool() ? (leaf->as_bool() ? 1.0 : 0.0) : param.default_value;
  }
  return leaf->is_number() ? leaf->as_number() : param.default_value;
}

uint8_t to_byte(double value) {
  const double clamped = std::min(255.0, std::max(0.0, value));
  return static_cast<uint8_t>(std::lround(clamped));
}

/* 预乘（+127 四舍五入，与合成内核的 c*a/255 口径一致）。 */
uint8_t premultiply(uint8_t color, uint8_t alpha) {
  return static_cast<uint8_t>((static_cast<uint32_t>(color) * alpha + 127u) / 255u);
}

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 0;
};

/* 读一组 `prefix.red/green/blue/alpha`（缺省值来自参数表，不在这里硬编码）。 */
Rgba read_color(const FillKindInfo& info, size_t first_index, const JsonValue& root) {
  Rgba color;
  color.r = to_byte(value_or_default(info, first_index + 0, root));
  color.g = to_byte(value_or_default(info, first_index + 1, root));
  color.b = to_byte(value_or_default(info, first_index + 2, root));
  color.a = to_byte(value_or_default(info, first_index + 3, root));
  return color;
}

}  // namespace

const FillKindInfo* fill_kind_info(uint32_t kind) {
  if (kind >= kFillKindCount || kind == kFillKindNone) return nullptr;
  return &kKinds[kind];
}

const char* fill_kind_name(uint32_t kind) {
  if (kind >= kFillKindCount) return nullptr;
  return kKinds[kind].name;
}

const char* fill_layer_default_name(uint32_t kind) {
  const FillKindInfo* info = fill_kind_info(kind);
  return info != nullptr ? info->layer_name : "";
}

bool fill_validate_settings(uint32_t kind, const std::string& settings_json, std::string* error) {
  const FillKindInfo* info = fill_kind_info(kind);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown fill kind";
    return false;
  }
  const std::string text = settings_json.empty() ? std::string("{}") : settings_json;

  std::string parse_error;
  const JsonValue root = json_parse(text, &parse_error);
  if (root.is_null() && !parse_error.empty()) {
    if (error != nullptr) *error = "invalid settings json: " + parse_error;
    return false;
  }
  if (!root.is_object()) {
    if (error != nullptr) *error = "fill settings must be a json object";
    return false;
  }

  for (const auto& entry : root.as_object()) {
    if (!root_allowed(*info, entry.first)) {
      if (error != nullptr) *error = "unknown settings key: " + entry.first;
      return false;
    }
  }

  for (size_t i = 0; i < info->param_count; ++i) {
    const FillParam& param = info->params[i];
    bool malformed = false;
    const JsonValue* leaf = locate_leaf(root, param.json_path, &malformed);
    if (malformed) {
      if (error != nullptr) *error = std::string("settings path is not an object: ") + param.json_path;
      return false;
    }
    if (leaf == nullptr) continue; /* 缺键 = 缺省值 */
    if (param.type == kFillParamBool) {
      if (!leaf->is_bool()) {
        if (error != nullptr) *error = std::string(param.json_path) + " must be a boolean";
        return false;
      }
      continue;
    }
    if (!leaf->is_number()) {
      if (error != nullptr) *error = std::string(param.json_path) + " must be a number";
      return false;
    }
    const double value = leaf->as_number();
    if (!std::isfinite(value) || value < param.min_value || value > param.max_value) {
      if (error != nullptr) {
        *error = std::string(param.json_path) + " out of range [" + number_to_text(param.min_value) +
                 ", " + number_to_text(param.max_value) + "]: " + number_to_text(value);
      }
      return false;
    }
  }
  return true;
}

std::string fill_default_settings_json(uint32_t kind) {
  const FillKindInfo* info = fill_kind_info(kind);
  if (info == nullptr || info->param_count == 0) return "{}";
  return json_dump(build_defaults(*info), 0);
}

bool fill_read_value(const std::string& settings_json, const char* json_path, double* out_number,
                     int32_t* out_boolean) {
  if (json_path == nullptr) return false;
  std::string parse_error;
  const JsonValue root = json_parse(settings_json.empty() ? std::string("{}") : settings_json,
                                    &parse_error);
  if (!root.is_object()) return false;
  bool malformed = false;
  const JsonValue* leaf = locate_leaf(root, json_path, &malformed);
  if (leaf == nullptr || malformed) return false;
  if (leaf->is_number()) {
    if (out_number != nullptr) *out_number = leaf->as_number();
    return true;
  }
  if (leaf->is_bool()) {
    if (out_boolean != nullptr) *out_boolean = leaf->as_bool() ? 1 : 0;
    return true;
  }
  return false;
}

bool fill_build_settings(uint32_t kind, const std::vector<std::string>& values,
                         std::string* out_json, std::string* error) {
  const FillKindInfo* info = fill_kind_info(kind);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown fill kind";
    return false;
  }
  if (values.size() != info->param_count) {
    if (error != nullptr) *error = "value count does not match parameter count";
    return false;
  }

  JsonValue root = build_defaults(*info);
  for (size_t i = 0; i < info->param_count; ++i) {
    const FillParam& param = info->params[i];
    if (values[i].empty()) continue;
    JsonValue leaf;
    if (param.type == kFillParamBool) {
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
    root = set_path(root, split_path(param.json_path), 0, std::move(leaf));
  }

  const std::string dumped = json_dump(root, 0);
  if (!fill_validate_settings(kind, dumped, error)) return false;
  if (out_json != nullptr) *out_json = dumped;
  return true;
}

bool fill_render(uint32_t kind, const std::string& settings_json, int32_t width, int32_t height,
                 ImageBuffer* out, std::string* error) {
  if (out == nullptr) {
    if (error != nullptr) *error = "out is null";
    return false;
  }
  *out = ImageBuffer{};
  if (!fill_validate_settings(kind, settings_json, error)) return false;
  if (!is_valid_canvas(comp_size{width, height})) {
    if (error != nullptr) *error = "fill size out of range";
    return false;
  }
  const FillKindInfo* info = fill_kind_info(kind);
  if (info == nullptr) {
    if (error != nullptr) *error = "unknown fill kind";
    return false;
  }

  std::string parse_error;
  const JsonValue root = json_parse(settings_json.empty() ? std::string("{}") : settings_json,
                                    &parse_error);

  out->width = width;
  out->height = height;
  out->format = COMP_PIXEL_RGBA8;
  std::vector<uint8_t>& bytes = out->writable_pixels();
  bytes.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);

  if (kind == kFillKindSolid) {
    const Rgba color = read_color(*info, 0, root);
    const uint32_t packed = static_cast<uint32_t>(premultiply(color.r, color.a)) |
                            (static_cast<uint32_t>(premultiply(color.g, color.a)) << 8) |
                            (static_cast<uint32_t>(premultiply(color.b, color.a)) << 16) |
                            (static_cast<uint32_t>(color.a) << 24);
    for (size_t i = 0; i < bytes.size(); i += 4u) {
      std::memcpy(bytes.data() + i, &packed, sizeof(packed));
    }
    return true;
  }

  if (kind == kFillKindGradient) {
    const Rgba start = read_color(*info, 0, root);
    const Rgba end = read_color(*info, 4, root);
    const double angle = value_or_default(*info, 8, root);
    const bool reverse = value_or_default(*info, 9, root) != 0.0;
    const double theta = angle * 3.14159265358979323846 / 180.0;
    /* 画布坐标 y 向下，而角度按 PS 的「逆时针为正」量：屏幕上是 y 取负。 */
    const double dx = std::cos(theta);
    const double dy = -std::sin(theta);
    const double p_min = std::min(0.0, width * dx) + std::min(0.0, height * dy);
    const double p_max = std::max(0.0, width * dx) + std::max(0.0, height * dy);
    const double span = p_max - p_min;
    const double sr = start.r, sg = start.g, sb = start.b, sa = start.a;
    const double er = end.r, eg = end.g, eb = end.b, ea = end.a;
    for (int32_t y = 0; y < height; ++y) {
      const double py = (static_cast<double>(y) + 0.5) * dy;
      for (int32_t x = 0; x < width; ++x) {
        double t = 0.0;
        if (span > 1e-12) {
          const double p = (static_cast<double>(x) + 0.5) * dx + py;
          t = (p - p_min) / span;
          t = std::min(1.0, std::max(0.0, t));
        }
        if (reverse) t = 1.0 - t;
        const double shade[4] = {sr + (er - sr) * t, sg + (eg - sg) * t, sb + (eb - sb) * t,
                                 sa + (ea - sa) * t};
        const uint8_t alpha = to_byte(shade[3]);
        const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                          static_cast<size_t>(x)) * 4u;
        bytes[i + 0] = premultiply(to_byte(shade[0]), alpha);
        bytes[i + 1] = premultiply(to_byte(shade[1]), alpha);
        bytes[i + 2] = premultiply(to_byte(shade[2]), alpha);
        bytes[i + 3] = alpha;
      }
    }
    return true;
  }

  /* kFillKindPattern */
  const int32_t pattern = static_cast<int32_t>(std::lround(value_or_default(*info, 0, root)));
  const int32_t size = std::max(1, static_cast<int32_t>(std::lround(value_or_default(*info, 1, root))));
  const Rgba foreground = read_color(*info, 2, root);
  const Rgba background = read_color(*info, 6, root);
  const uint32_t fg = static_cast<uint32_t>(premultiply(foreground.r, foreground.a)) |
                      (static_cast<uint32_t>(premultiply(foreground.g, foreground.a)) << 8) |
                      (static_cast<uint32_t>(premultiply(foreground.b, foreground.a)) << 16) |
                      (static_cast<uint32_t>(foreground.a) << 24);
  const uint32_t bg = static_cast<uint32_t>(premultiply(background.r, background.a)) |
                      (static_cast<uint32_t>(premultiply(background.g, background.a)) << 8) |
                      (static_cast<uint32_t>(premultiply(background.b, background.a)) << 16) |
                      (static_cast<uint32_t>(background.a) << 24);
  const double half = static_cast<double>(size) / 2.0;
  const double radius = static_cast<double>(size) / 4.0;
  const double radius_sq = radius * radius;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      bool use_foreground = false;
      switch (pattern) {
        case 0: /* 棋盘 */
          use_foreground = ((x / size) + (y / size)) % 2 != 0;
          break;
        case 1: /* 竖条 */
          use_foreground = (x / size) % 2 != 0;
          break;
        case 2: /* 横条 */
          use_foreground = (y / size) % 2 != 0;
          break;
        default: { /* 圆点：格心半径内为前景 */
          const double cdx = static_cast<double>(x % size) + 0.5 - half;
          const double cdy = static_cast<double>(y % size) + 0.5 - half;
          use_foreground = cdx * cdx + cdy * cdy <= radius_sq;
          break;
        }
      }
      const uint32_t packed = use_foreground ? fg : bg;
      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                        static_cast<size_t>(x)) * 4u;
      std::memcpy(bytes.data() + i, &packed, sizeof(packed));
    }
  }
  return true;
}

}  // namespace compositor

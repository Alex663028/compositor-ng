// AI生成
/*
 * camera_raw.cpp — Camera Raw 的设置解析、归一化、曲线与管线。
 *
 * 与上游的对应关系逐条写在函数上方。像素公式一律调用 `imaging/AdjustPixels.h`
 * 的既有内核；本文件只负责“按上游顺序、按上游参数”把它们串起来。
 */
#include "domain/camera_raw.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

#include "engine.h"
#include "format/comp_json.h"
#include "imaging/AdjustPixels.h"

namespace compositor {
namespace {

/* 上游 `PixelFilter.lensStrength`（Filters.swift:238）。 */
constexpr double kCameraRawLensStrength = 0.35;

double clampd(double value, double low, double high) {
  return value < low ? low : (value > high ? high : value);
}

std::string qualify(const std::string& where, const char* key) {
  return where.empty() ? std::string(key) : where + "." + key;
}

bool reject_unknown_keys(const JsonValue& object, std::initializer_list<const char*> allowed,
                         const std::string& where, std::string* error) {
  for (const auto& entry : object.as_object()) {
    bool known = false;
    for (const char* key : allowed) {
      if (entry.first == key) {
        known = true;
        break;
      }
    }
    if (!known) {
      if (error != nullptr) *error = "camera raw: unknown key " + qualify(where, entry.first.c_str());
      return false;
    }
  }
  return true;
}

bool object_member(const JsonValue& object, const char* key, const std::string& where,
                   const JsonValue** out, std::string* error) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) {
    *out = nullptr;
    return true;
  }
  if (!value->is_object()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, key) + " must be an object";
    return false;
  }
  *out = value;
  return true;
}

bool number_field(const JsonValue& object, const char* key, const std::string& where, double low,
                  double high, double* value, std::string* error) {
  const JsonValue* field = object.find(key);
  if (field == nullptr) return true;
  if (!field->is_number()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, key) + " must be a number";
    return false;
  }
  const double number = field->as_number();
  if (!std::isfinite(number) || number < low || number > high) {
    if (error != nullptr) {
      *error = "camera raw: " + qualify(where, key) + " is out of range";
    }
    return false;
  }
  *value = number;
  return true;
}

bool bool_field(const JsonValue& object, const char* key, const std::string& where, bool* value,
                std::string* error) {
  const JsonValue* field = object.find(key);
  if (field == nullptr) return true;
  if (!field->is_bool()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, key) + " must be a boolean";
    return false;
  }
  *value = field->as_bool();
  return true;
}

/* 枚举字符串：名字数组与上游 rawValue 逐字一致。 */
bool choice_field(const JsonValue& object, const char* key, const std::string& where,
                  const char* const* names, int count, int* value, std::string* error) {
  const JsonValue* field = object.find(key);
  if (field == nullptr) return true;
  if (!field->is_string()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, key) + " must be a string";
    return false;
  }
  const std::string text = field->as_string();
  for (int i = 0; i < count; ++i) {
    if (text == names[i]) {
      *value = i;
      return true;
    }
  }
  if (error != nullptr) {
    *error = "camera raw: " + qualify(where, key) + " has an unknown value " + text;
  }
  return false;
}

bool eight_numbers(const JsonValue& object, const char* key, const std::string& where,
                   std::array<double, 8>* values, std::string* error) {
  const JsonValue* field = object.find(key);
  if (field == nullptr) return true;
  if (!field->is_array() || field->as_array().size() != 8) {
    if (error != nullptr) {
      *error = "camera raw: " + qualify(where, key) + " must be an array of eight numbers";
    }
    return false;
  }
  for (size_t i = 0; i < 8; ++i) {
    const JsonValue& element = field->as_array()[i];
    if (!element.is_number() || !std::isfinite(element.as_number())) {
      if (error != nullptr) {
        *error = "camera raw: " + qualify(where, key) + " has a non-numeric entry";
      }
      return false;
    }
    const double number = element.as_number();
    if (number < -100.0 || number > 100.0) {
      if (error != nullptr) {
        *error = "camera raw: " + qualify(where, key) + " is out of range";
      }
      return false;
    }
    (*values)[i] = number;
  }
  return true;
}

bool curve_points_from_json(const JsonValue& array, const std::string& path,
                            std::vector<CameraRawCurvePoint>* points, std::string* error) {
  if (!array.is_array()) {
    if (error != nullptr) *error = "camera raw: " + path + " must be an array of points";
    return false;
  }
  points->clear();
  const JsonValue::Array& entries = array.as_array();
  for (size_t i = 0; i < entries.size(); ++i) {
    const JsonValue& entry = entries[i];
    if (!entry.is_object()) {
      if (error != nullptr) *error = "camera raw: " + path + " must contain objects";
      return false;
    }
    if (!reject_unknown_keys(entry, {"x", "y"}, path, error)) return false;
    const JsonValue* x = entry.find("x");
    const JsonValue* y = entry.find("y");
    if (x == nullptr || y == nullptr || !x->is_number() || !y->is_number() ||
        !std::isfinite(x->as_number()) || !std::isfinite(y->as_number())) {
      if (error != nullptr) *error = "camera raw: " + path + " needs finite x and y";
      return false;
    }
    CameraRawCurvePoint point;
    point.x = x->as_number();
    point.y = y->as_number();
    points->push_back(point);
  }
  return true;
}

bool parse_curve(const JsonValue& object, const std::string& where, CameraRawCurveSettings* out,
                 std::string* error) {
  if (!reject_unknown_keys(object,
                           {"shadows", "darks", "lights", "highlights", "shadowSplit", "darkSplit",
                            "lightSplit", "refineSaturation", "rgb", "red", "green", "blue"},
                           where, error)) {
    return false;
  }
  if (!number_field(object, "shadows", where, -100.0, 100.0, &out->shadows, error) ||
      !number_field(object, "darks", where, -100.0, 100.0, &out->darks, error) ||
      !number_field(object, "lights", where, -100.0, 100.0, &out->lights, error) ||
      !number_field(object, "highlights", where, -100.0, 100.0, &out->highlights, error) ||
      !number_field(object, "shadowSplit", where, 0.0, 100.0, &out->shadow_split, error) ||
      !number_field(object, "darkSplit", where, 0.0, 100.0, &out->dark_split, error) ||
      !number_field(object, "lightSplit", where, 0.0, 100.0, &out->light_split, error) ||
      !number_field(object, "refineSaturation", where, -100.0, 100.0, &out->refine_saturation,
                    error)) {
    return false;
  }
  struct Channel {
    const char* key;
    std::vector<CameraRawCurvePoint>* points;
  };
  const Channel channels[4] = {{"rgb", &out->rgb},
                               {"red", &out->red},
                               {"green", &out->green},
                               {"blue", &out->blue}};
  for (const Channel& channel : channels) {
    const JsonValue* field = object.find(channel.key);
    if (field == nullptr) continue;
    if (!curve_points_from_json(*field, qualify(where, channel.key), channel.points, error)) {
      return false;
    }
  }
  return true;
}

bool parse_point_color(const JsonValue& object, const std::string& path,
                       CameraRawPointColor* out, std::string* error) {
  if (!reject_unknown_keys(object,
                           {"hue", "saturation", "luminance", "hueShift", "saturationShift",
                            "luminanceShift", "hueRange", "saturationRange", "luminanceRange",
                            "visualize"},
                           path, error)) {
    return false;
  }
  return number_field(object, "hue", path, 0.0, 360.0, &out->hue, error) &&
         number_field(object, "saturation", path, 0.0, 1.0, &out->saturation, error) &&
         number_field(object, "luminance", path, 0.0, 1.0, &out->luminance, error) &&
         number_field(object, "hueShift", path, -100.0, 100.0, &out->hue_shift, error) &&
         number_field(object, "saturationShift", path, -100.0, 100.0, &out->saturation_shift,
                      error) &&
         number_field(object, "luminanceShift", path, -100.0, 100.0, &out->luminance_shift,
                      error) &&
         number_field(object, "hueRange", path, 5.0, 180.0, &out->hue_range, error) &&
         number_field(object, "saturationRange", path, 0.05, 1.0, &out->saturation_range, error) &&
         number_field(object, "luminanceRange", path, 0.05, 1.0, &out->luminance_range, error) &&
         bool_field(object, "visualize", path, &out->visualize, error);
}

bool parse_mixer(const JsonValue& object, const std::string& where, CameraRawMixerSettings* out,
                 std::string* error) {
  if (!reject_unknown_keys(object, {"hue", "saturation", "luminance", "points"}, where, error)) {
    return false;
  }
  if (!eight_numbers(object, "hue", where, &out->hue, error) ||
      !eight_numbers(object, "saturation", where, &out->saturation, error) ||
      !eight_numbers(object, "luminance", where, &out->luminance, error)) {
    return false;
  }
  const JsonValue* points = object.find("points");
  if (points == nullptr) return true;
  if (!points->is_array()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, "points") + " must be an array";
    return false;
  }
  out->points.clear();
  const JsonValue::Array& entries = points->as_array();
  /* 上游 `points.prefix(8)`：超过 8 个是截断，不是错误。 */
  const size_t count = entries.size() < 8 ? entries.size() : 8;
  for (size_t i = 0; i < count; ++i) {
    if (!entries[i].is_object()) {
      if (error != nullptr) {
        *error = "camera raw: " + qualify(where, "points") + " must contain objects";
      }
      return false;
    }
    CameraRawPointColor point;
    if (!parse_point_color(entries[i], qualify(where, "points"), &point, error)) return false;
    out->points.push_back(point);
  }
  return true;
}

bool parse_grade_wheel(const JsonValue& object, const std::string& where,
                       CameraRawGradeWheel* out, std::string* error) {
  if (!reject_unknown_keys(object, {"hue", "saturation", "luminance"}, where, error)) {
    return false;
  }
  return number_field(object, "hue", where, 0.0, 360.0, &out->hue, error) &&
         number_field(object, "saturation", where, 0.0, 100.0, &out->saturation, error) &&
         number_field(object, "luminance", where, -100.0, 100.0, &out->luminance, error);
}

bool parse_grading(const JsonValue& object, const std::string& where,
                   CameraRawGradingSettings* out, std::string* error) {
  if (!reject_unknown_keys(
          object, {"shadows", "midtones", "highlights", "global", "blending", "balance"}, where,
          error)) {
    return false;
  }
  struct Wheel {
    const char* key;
    CameraRawGradeWheel* wheel;
  };
  const Wheel wheels[4] = {{"shadows", &out->shadows},
                           {"midtones", &out->midtones},
                           {"highlights", &out->highlights},
                           {"global", &out->global}};
  for (const Wheel& entry : wheels) {
    const JsonValue* field = object.find(entry.key);
    if (field == nullptr) continue;
    if (!field->is_object()) {
      if (error != nullptr) *error = "camera raw: " + qualify(where, entry.key) + " must be an object";
      return false;
    }
    if (!parse_grade_wheel(*field, qualify(where, entry.key), entry.wheel, error)) return false;
  }
  return number_field(object, "blending", where, 0.0, 100.0, &out->blending, error) &&
         number_field(object, "balance", where, -100.0, 100.0, &out->balance, error);
}

bool parse_detail(const JsonValue& object, const std::string& where, CameraRawDetailSettings* out,
                  std::string* error) {
  if (!reject_unknown_keys(object,
                           {"sharpenAmount", "sharpenRadius", "sharpenDetail", "sharpenMasking",
                            "noiseLuminance", "noiseLuminanceDetail", "noiseLuminanceContrast",
                            "noiseColor", "noiseColorDetail", "noiseColorSmoothness"},
                           where, error)) {
    return false;
  }
  return number_field(object, "sharpenAmount", where, 0.0, 150.0, &out->sharpen_amount, error) &&
         number_field(object, "sharpenRadius", where, 0.0, 100.0, &out->sharpen_radius, error) &&
         number_field(object, "sharpenDetail", where, 0.0, 100.0, &out->sharpen_detail, error) &&
         number_field(object, "sharpenMasking", where, 0.0, 100.0, &out->sharpen_masking, error) &&
         number_field(object, "noiseLuminance", where, 0.0, 100.0, &out->noise_luminance, error) &&
         number_field(object, "noiseLuminanceDetail", where, 0.0, 100.0,
                      &out->noise_luminance_detail, error) &&
         number_field(object, "noiseLuminanceContrast", where, 0.0, 100.0,
                      &out->noise_luminance_contrast, error) &&
         number_field(object, "noiseColor", where, 0.0, 100.0, &out->noise_color, error) &&
         number_field(object, "noiseColorDetail", where, 0.0, 100.0, &out->noise_color_detail,
                      error) &&
         number_field(object, "noiseColorSmoothness", where, 0.0, 100.0,
                      &out->noise_color_smoothness, error);
}

bool parse_optics(const JsonValue& object, const std::string& where, CameraRawOpticsSettings* out,
                  std::string* error) {
  if (!reject_unknown_keys(object,
                           {"removeChromaticAberration", "enableLensProfile", "profileDistortion",
                            "profileVignetting", "distortion", "purpleAmount", "purpleHueLow",
                            "purpleHueHigh", "greenAmount", "greenHueLow", "greenHueHigh",
                            "vignetteAmount", "vignetteMidpoint"},
                           where, error)) {
    return false;
  }
  return bool_field(object, "removeChromaticAberration", where, &out->remove_chromatic_aberration,
                    error) &&
         bool_field(object, "enableLensProfile", where, &out->enable_lens_profile, error) &&
         number_field(object, "profileDistortion", where, 0.0, 100.0, &out->profile_distortion,
                      error) &&
         number_field(object, "profileVignetting", where, 0.0, 100.0, &out->profile_vignetting,
                      error) &&
         number_field(object, "distortion", where, -100.0, 100.0, &out->distortion, error) &&
         number_field(object, "purpleAmount", where, 0.0, 100.0, &out->purple_amount, error) &&
         number_field(object, "purpleHueLow", where, 0.0, 360.0, &out->purple_hue_low, error) &&
         number_field(object, "purpleHueHigh", where, 0.0, 360.0, &out->purple_hue_high, error) &&
         number_field(object, "greenAmount", where, 0.0, 100.0, &out->green_amount, error) &&
         number_field(object, "greenHueLow", where, 0.0, 360.0, &out->green_hue_low, error) &&
         number_field(object, "greenHueHigh", where, 0.0, 360.0, &out->green_hue_high, error) &&
         number_field(object, "vignetteAmount", where, -100.0, 100.0, &out->vignette_amount, error) &&
         number_field(object, "vignetteMidpoint", where, 0.0, 100.0, &out->vignette_midpoint,
                      error);
}

bool parse_geometry(const JsonValue& object, const std::string& where,
                    CameraRawGeometrySettings* out, std::string* error) {
  if (!reject_unknown_keys(object,
                           {"upright", "projection", "vertical", "horizontal", "rotate", "aspect",
                            "scale", "offsetX", "offsetY", "constrainCrop", "guides"},
                           where, error)) {
    return false;
  }
  static const char* const kUprightNames[] = {"Off", "Guided"};
  static const char* const kProjectionNames[] = {"Perspective", "Rectilinear"};
  int upright = 0, projection = 0;
  if (!choice_field(object, "upright", where, kUprightNames, 2, &upright, error) ||
      !choice_field(object, "projection", where, kProjectionNames, 2, &projection, error)) {
    return false;
  }
  out->upright = static_cast<CameraRawUpright>(upright);
  out->projection = static_cast<CameraRawProjection>(projection);
  if (!number_field(object, "vertical", where, -100.0, 100.0, &out->vertical, error) ||
      !number_field(object, "horizontal", where, -100.0, 100.0, &out->horizontal, error) ||
      !number_field(object, "rotate", where, -45.0, 45.0, &out->rotate, error) ||
      !number_field(object, "aspect", where, -100.0, 100.0, &out->aspect, error) ||
      !number_field(object, "scale", where, -100.0, 100.0, &out->scale, error) ||
      !number_field(object, "offsetX", where, -100.0, 100.0, &out->offset_x, error) ||
      !number_field(object, "offsetY", where, -100.0, 100.0, &out->offset_y, error) ||
      !bool_field(object, "constrainCrop", where, &out->constrain_crop, error)) {
    return false;
  }
  const JsonValue* guides = object.find("guides");
  if (guides == nullptr) return true;
  if (!guides->is_array()) {
    if (error != nullptr) *error = "camera raw: " + qualify(where, "guides") + " must be an array";
    return false;
  }
  out->guides.clear();
  for (const JsonValue& entry : guides->as_array()) {
    if (!entry.is_object()) {
      if (error != nullptr) {
        *error = "camera raw: " + qualify(where, "guides") + " must contain objects";
      }
      return false;
    }
    if (!reject_unknown_keys(entry, {"startX", "startY", "endX", "endY"},
                             qualify(where, "guides"), error)) {
      return false;
    }
    CameraRawGeometryGuide guide;
    if (!number_field(entry, "startX", qualify(where, "guides"), -1e6, 1e6, &guide.start_x, error) ||
        !number_field(entry, "startY", qualify(where, "guides"), -1e6, 1e6, &guide.start_y, error) ||
        !number_field(entry, "endX", qualify(where, "guides"), -1e6, 1e6, &guide.end_x, error) ||
        !number_field(entry, "endY", qualify(where, "guides"), -1e6, 1e6, &guide.end_y, error)) {
      return false;
    }
    out->guides.push_back(guide);
  }
  return true;
}

bool parse_calibration(const JsonValue& object, const std::string& where,
                       CameraRawCalibrationSettings* out, std::string* error) {
  if (!reject_unknown_keys(object,
                           {"process", "shadowTint", "redHue", "redSaturation", "greenHue",
                            "greenSaturation", "blueHue", "blueSaturation"},
                           where, error)) {
    return false;
  }
  static const char* const kProcessNames[] = {"Version 1", "Version 2", "Version 3",
                                              "Version 4", "Version 5", "Version 6"};
  int process = 5;
  if (!choice_field(object, "process", where, kProcessNames, 6, &process, error)) return false;
  out->process = static_cast<CameraRawProcess>(process + 1);
  return number_field(object, "shadowTint", where, -100.0, 100.0, &out->shadow_tint, error) &&
         number_field(object, "redHue", where, -100.0, 100.0, &out->red_hue, error) &&
         number_field(object, "redSaturation", where, -100.0, 100.0, &out->red_saturation, error) &&
         number_field(object, "greenHue", where, -100.0, 100.0, &out->green_hue, error) &&
         number_field(object, "greenSaturation", where, -100.0, 100.0, &out->green_saturation,
                      error) &&
         number_field(object, "blueHue", where, -100.0, 100.0, &out->blue_hue, error) &&
         number_field(object, "blueSaturation", where, -100.0, 100.0, &out->blue_saturation,
                      error);
}

/* 上游 `repair(_:)`：排序、强制端点、收内点间距。 */
void repair_curve_points(std::vector<CameraRawCurvePoint>* points) {
  std::vector<CameraRawCurvePoint> sorted = *points;
  sorted.erase(std::remove_if(sorted.begin(), sorted.end(),
                              [](const CameraRawCurvePoint& point) {
                                return !std::isfinite(point.x) || !std::isfinite(point.y);
                              }),
               sorted.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const CameraRawCurvePoint& a, const CameraRawCurvePoint& b) { return a.x < b.x; });
  if (sorted.size() < 2) {
    points->assign({{0.0, 0.0}, {1.0, 1.0}});
    return;
  }
  sorted.front().x = 0.0;
  sorted.front().y = clampd(sorted.front().y, 0.0, 1.0);
  sorted.back().x = 1.0;
  sorted.back().y = clampd(sorted.back().y, 0.0, 1.0);
  std::vector<CameraRawCurvePoint> kept;
  kept.push_back(sorted.front());
  for (size_t i = 1; i + 1 < sorted.size(); ++i) {
    const double x = clampd(sorted[i].x, 0.01, 0.99);
    if (x <= kept.back().x + 0.01) continue;
    kept.push_back({x, clampd(sorted[i].y, 0.0, 1.0)});
  }
  kept.push_back(sorted.back());
  points->swap(kept);
}

/* 上 游 `CameraRawPointColor.normalized`。 */
CameraRawPointColor normalized_point(const CameraRawPointColor& point) {
  CameraRawPointColor result = point;
  result.hue = clampd(point.hue, 0.0, 360.0);
  result.saturation = clampd(point.saturation, 0.0, 1.0);
  result.luminance = clampd(point.luminance, 0.0, 1.0);
  result.hue_shift = clampd(point.hue_shift, -100.0, 100.0);
  result.saturation_shift = clampd(point.saturation_shift, -100.0, 100.0);
  result.luminance_shift = clampd(point.luminance_shift, -100.0, 100.0);
  result.hue_range = clampd(point.hue_range, 5.0, 180.0);
  result.saturation_range = clampd(point.saturation_range, 0.05, 1.0);
  result.luminance_range = clampd(point.luminance_range, 0.05, 1.0);
  return result;
}

/* `adjust_camera_raw_curve_color` 要求四个 LUT 都非空；缺省是恒等表。 */
struct CurveTables {
  std::array<float, 256> tone{};
  std::array<float, 256> red{};
  std::array<float, 256> green{};
  std::array<float, 256> blue{};
};

CurveTables curve_tables(const CameraRawCurveSettings& curve) {
  CurveTables tables;
  tables.tone = curve.tone_table();
  tables.red = CameraRawCurveSettings::channel_table(curve.red);
  tables.green = CameraRawCurveSettings::channel_table(curve.green);
  tables.blue = CameraRawCurveSettings::channel_table(curve.blue);
  return tables;
}

}  // namespace

/* ============================================================ 曲线求值 */

double camera_raw_curve_value(const std::vector<CameraRawCurvePoint>& points, double x) {
  if (points.size() < 2) return x;
  const size_t last = points.size() - 1;
  size_t index = 0;
  for (size_t i = 0; i <= last; ++i) {
    if (points[i].x <= x) index = i;
  }
  index = std::min(last - 1, index);
  std::vector<double> slopes(last);
  for (size_t i = 0; i < last; ++i) {
    const double span = points[i + 1].x - points[i].x;
    slopes[i] = span != 0.0 ? (points[i + 1].y - points[i].y) / span : 0.0;
  }
  auto slope = [&](size_t i) -> double {
    if (i == 0) return slopes[0];
    if (i == last) return slopes[last - 1];
    if (slopes[i - 1] * slopes[i] <= 0.0) return 0.0;
    return 2.0 / (1.0 / slopes[i - 1] + 1.0 / slopes[i]);
  };
  const double h = points[index + 1].x - points[index].x;
  const double t = clampd(h != 0.0 ? (x - points[index].x) / h : 0.0, 0.0, 1.0);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double y = (2.0 * t3 - 3.0 * t2 + 1.0) * points[index].y +
                   (t3 - 2.0 * t2 + t) * h * slope(index) +
                   (-2.0 * t3 + 3.0 * t2) * points[index + 1].y +
                   (t3 - t2) * h * slope(index + 1);
  return clampd(y, 0.0, 1.0);
}

/* ============================================================ 设置 */

CameraRawCurveSettings::CameraRawCurveSettings() {
  const std::vector<CameraRawCurvePoint> linear = {{0.0, 0.0}, {1.0, 1.0}};
  rgb = linear;
  red = linear;
  green = linear;
  blue = linear;
}

bool CameraRawCurveSettings::adjusts() const {
  auto linear = [](const std::vector<CameraRawCurvePoint>& points) {
    return points.size() == 2 && points[0].x == 0.0 && points[0].y == 0.0 &&
           points[1].x == 1.0 && points[1].y == 1.0;
  };
  return shadows != 0.0 || darks != 0.0 || lights != 0.0 || highlights != 0.0 ||
         refine_saturation != 0.0 || !linear(rgb) || !linear(red) || !linear(green) ||
         !linear(blue);
}

bool CameraRawCurveSettings::has_measured_dividers() const {
  return shadow_split == 25.0 && dark_split == 50.0 && light_split == 75.0 &&
         refine_saturation == 0.0;
}

double CameraRawCurveSettings::parametric(double tone) const {
  if (shadows == 0.0 && darks == 0.0 && lights == 0.0 && highlights == 0.0) return tone;
  const double strength = 1.66;
  auto bend = [&](double value, double lower, double low, double upper, double high) {
    if (value < lower && lower > 0.0) {
      return lower * std::pow(value / lower, std::pow(2.0, -low / 100.0 * strength));
    }
    if (value > upper && upper < 1.0) {
      const double rest = 1.0 - upper;
      return 1.0 - rest * std::pow((1.0 - value) / rest, std::pow(2.0, high / 100.0 * strength));
    }
    return value;
  };
  std::vector<CameraRawCurvePoint> anchors;
  anchors.reserve(33);
  for (int index = 0; index <= 32; ++index) {
    const double x = static_cast<double>(index) / 32.0;
    const double y = bend(bend(x, shadow_split / 100.0, shadows, light_split / 100.0, highlights),
                          dark_split / 100.0, darks, dark_split / 100.0, lights);
    anchors.push_back({x, y});
  }
  return camera_raw_curve_value(anchors, tone);
}

std::array<float, 256> CameraRawCurveSettings::tone_table() const {
  std::array<float, 256> table{};
  for (int i = 0; i < 256; ++i) {
    const double x = static_cast<double>(i) / 255.0;
    table[static_cast<size_t>(i)] = static_cast<float>(
        camera_raw_curve_value(rgb, has_measured_dividers() ? x : parametric(x)));
  }
  return table;
}

std::array<float, 256> CameraRawCurveSettings::channel_table(
    const std::vector<CameraRawCurvePoint>& points) {
  std::array<float, 256> table{};
  for (int i = 0; i < 256; ++i) {
    table[static_cast<size_t>(i)] =
        static_cast<float>(camera_raw_curve_value(points, static_cast<double>(i) / 255.0));
  }
  return table;
}

void CameraRawCurveSettings::normalize() {
  shadows = clampd(shadows, -100.0, 100.0);
  darks = clampd(darks, -100.0, 100.0);
  lights = clampd(lights, -100.0, 100.0);
  highlights = clampd(highlights, -100.0, 100.0);
  refine_saturation = clampd(refine_saturation, -100.0, 100.0);
  shadow_split = clampd(shadow_split, 5.0, 90.0);
  dark_split = clampd(dark_split, shadow_split + 2.0, 95.0);
  light_split = clampd(light_split, dark_split + 2.0, 98.0);
  repair_curve_points(&rgb);
  repair_curve_points(&red);
  repair_curve_points(&green);
  repair_curve_points(&blue);
}

bool CameraRawMixerSettings::adjusts() const {
  for (size_t i = 0; i < 8; ++i) {
    if (hue[i] != 0.0 || saturation[i] != 0.0 || luminance[i] != 0.0) return true;
  }
  for (const CameraRawPointColor& point : points) {
    if (point.hue_shift != 0.0 || point.saturation_shift != 0.0 ||
        point.luminance_shift != 0.0) {
      return true;
    }
  }
  return false;
}

std::array<float, 24> CameraRawMixerSettings::mixer_floats() const {
  std::array<float, 24> values{};
  for (size_t i = 0; i < 8; ++i) {
    values[i] = static_cast<float>(hue[i] / 100.0);
    values[8 + i] = static_cast<float>(saturation[i] / 100.0);
    values[16 + i] = static_cast<float>(luminance[i] / 100.0);
  }
  return values;
}

std::vector<float> CameraRawMixerSettings::point_floats() const {
  std::vector<float> values;
  values.reserve(points.size() * 9);
  for (const CameraRawPointColor& point : points) {
    values.push_back(static_cast<float>(point.hue / 360.0));
    values.push_back(static_cast<float>(point.saturation));
    values.push_back(static_cast<float>(point.luminance));
    values.push_back(static_cast<float>(point.hue_shift / 100.0));
    values.push_back(static_cast<float>(point.saturation_shift / 100.0));
    values.push_back(static_cast<float>(point.luminance_shift / 100.0));
    values.push_back(static_cast<float>(point.hue_range / 360.0));
    values.push_back(static_cast<float>(point.saturation_range));
    values.push_back(static_cast<float>(point.luminance_range));
  }
  return values;
}

void CameraRawMixerSettings::normalize() {
  for (size_t i = 0; i < 8; ++i) {
    hue[i] = clampd(hue[i], -100.0, 100.0);
    saturation[i] = clampd(saturation[i], -100.0, 100.0);
    luminance[i] = clampd(luminance[i], -100.0, 100.0);
  }
  if (points.size() > 8) points.resize(8);
  for (CameraRawPointColor& point : points) point = normalized_point(point);
}

std::array<CameraRawGradeWheel, 4> CameraRawGradingSettings::wheels() const {
  return {shadows, midtones, highlights, global};
}

bool CameraRawGradingSettings::adjusts() const {
  for (const CameraRawGradeWheel& wheel : wheels()) {
    if (wheel.saturation != 0.0 || wheel.luminance != 0.0) return true;
  }
  return false;
}

void CameraRawGradingSettings::normalize() {
  for (CameraRawGradeWheel* wheel : {&shadows, &midtones, &highlights, &global}) {
    wheel->hue = clampd(wheel->hue, 0.0, 360.0);
    wheel->saturation = clampd(wheel->saturation, 0.0, 100.0);
    wheel->luminance = clampd(wheel->luminance, -100.0, 100.0);
  }
  blending = clampd(blending, 0.0, 100.0);
  balance = clampd(balance, -100.0, 100.0);
}

bool CameraRawDetailSettings::adjusts() const {
  return sharpen_amount != 0.0 || noise_luminance != 0.0 || noise_color != 0.0;
}

bool CameraRawOpticsSettings::adjusts() const {
  return remove_chromatic_aberration || enable_lens_profile || distortion != 0.0 ||
         purple_amount != 0.0 || green_amount != 0.0 || vignette_amount != 0.0;
}

double CameraRawOpticsSettings::distortion_k(double profile_strength) const {
  const double manual = distortion / 100.0 * profile_strength;
  const double profile = enable_lens_profile ? profile_distortion / 100.0 * profile_strength : 0.0;
  return manual + profile;
}

void CameraRawOpticsSettings::normalize() {
  profile_distortion = clampd(profile_distortion, 0.0, 100.0);
  profile_vignetting = clampd(profile_vignetting, 0.0, 100.0);
  distortion = clampd(distortion, -100.0, 100.0);
  purple_amount = clampd(purple_amount, 0.0, 100.0);
  green_amount = clampd(green_amount, 0.0, 100.0);
  vignette_amount = clampd(vignette_amount, -100.0, 100.0);
  vignette_midpoint = clampd(vignette_midpoint, 0.0, 100.0);
  purple_hue_low = clampd(purple_hue_low, 0.0, 360.0);
  purple_hue_high = clampd(purple_hue_high, 0.0, 360.0);
  green_hue_low = clampd(green_hue_low, 0.0, 360.0);
  green_hue_high = clampd(green_hue_high, 0.0, 360.0);
  if (purple_hue_low > purple_hue_high) std::swap(purple_hue_low, purple_hue_high);
  if (green_hue_low > green_hue_high) std::swap(green_hue_low, green_hue_high);
}

bool CameraRawGeometrySettings::adjusts() const {
  bool uses_guides = false;
  if (upright == CameraRawUpright::kGuided) {
    for (const CameraRawGeometryGuide& guide : guides) {
      if (std::hypot(guide.end_x - guide.start_x, guide.end_y - guide.start_y) > 0.01) {
        uses_guides = true;
        break;
      }
    }
  }
  return uses_guides || vertical != 0.0 || horizontal != 0.0 || rotate != 0.0 ||
         aspect != 0.0 || scale != 0.0 || offset_x != 0.0 || offset_y != 0.0;
}

void CameraRawGeometrySettings::normalize() {
  vertical = clampd(vertical, -100.0, 100.0);
  horizontal = clampd(horizontal, -100.0, 100.0);
  rotate = clampd(rotate, -45.0, 45.0);
  aspect = clampd(aspect, -100.0, 100.0);
  scale = clampd(scale, -100.0, 100.0);
  offset_x = clampd(offset_x, -100.0, 100.0);
  offset_y = clampd(offset_y, -100.0, 100.0);
  guides.erase(std::remove_if(guides.begin(), guides.end(),
                              [](const CameraRawGeometryGuide& guide) {
                                return !(std::hypot(guide.end_x - guide.start_x,
                                                    guide.end_y - guide.start_y) > 0.01);
                              }),
               guides.end());
}

bool CameraRawCalibrationSettings::adjusts() const {
  return shadow_tint != 0.0 || red_hue != 0.0 || red_saturation != 0.0 || green_hue != 0.0 ||
         green_saturation != 0.0 || blue_hue != 0.0 || blue_saturation != 0.0;
}

bool CameraRawSettings::adjusts_light() const {
  return exposure != 0.0 || contrast != 0.0 || highlights != 0.0 || shadows != 0.0 ||
         whites != 0.0 || blacks != 0.0;
}

bool CameraRawSettings::adjusts_color() const {
  return temperature != 0.0 || tint != 0.0 || vibrance != 0.0 || saturation != 0.0;
}

bool CameraRawSettings::adjusts_effects() const {
  return texture != 0.0 || clarity != 0.0 || dehaze != 0.0 || glow != 0.0 ||
         vignette_amount != 0.0 || grain_amount != 0.0;
}

bool CameraRawSettings::is_identity() const {
  return !adjusts_light() && !adjusts_color() && !adjusts_effects() && !curve.adjusts() &&
         !mixer.adjusts() && !grading.adjusts() && !detail.adjusts() && !optics.adjusts() &&
         !geometry.adjusts() && !calibration.adjusts();
}

double CameraRawSettings::grain_kernel_size() const {
  return 0.5 + grain_size / 100.0 * 19.5;
}

void CameraRawSettings::normalize() {
  exposure = clampd(exposure, -5.0, 5.0);
  for (double* value : {&contrast, &highlights, &shadows, &whites, &blacks, &temperature, &tint,
                        &vibrance, &saturation, &texture, &clarity, &dehaze, &glow_range,
                        &glow_spread, &glow_warmth, &vignette_amount, &vignette_roundness}) {
    *value = clampd(*value, -100.0, 100.0);
  }
  for (double* value : {&glow, &vignette_midpoint, &vignette_feather, &vignette_highlights,
                        &grain_amount, &grain_size, &grain_roughness}) {
    *value = clampd(*value, 0.0, 100.0);
  }
  curve.normalize();
  mixer.normalize();
  grading.normalize();
  optics.normalize();
  geometry.normalize();

  CameraRawDetailSettings& d = detail;
  d.sharpen_amount = clampd(d.sharpen_amount, 0.0, 150.0);
  for (double* value : {&d.sharpen_radius, &d.sharpen_detail, &d.sharpen_masking,
                        &d.noise_luminance, &d.noise_luminance_detail,
                        &d.noise_luminance_contrast, &d.noise_color, &d.noise_color_detail,
                        &d.noise_color_smoothness}) {
    *value = clampd(*value, 0.0, 100.0);
  }

  CameraRawCalibrationSettings& c = calibration;
  for (double* value : {&c.shadow_tint, &c.red_hue, &c.red_saturation, &c.green_hue,
                        &c.green_saturation, &c.blue_hue, &c.blue_saturation}) {
    *value = clampd(*value, -100.0, 100.0);
  }
}

/* ============================================================ JSON */

bool camera_raw_settings_from_json(const std::string& text, CameraRawSettings* settings,
                                   std::string* error) {
  if (settings == nullptr) {
    if (error != nullptr) *error = "camera raw: settings output is null";
    return false;
  }
  std::string parse_error;
  const std::string body = text.empty() ? std::string("{}") : text;
  const JsonValue root = json_parse(body, &parse_error);
  if (root.is_null() && !parse_error.empty()) {
    if (error != nullptr) *error = "camera raw: invalid settings json: " + parse_error;
    return false;
  }
  if (!root.is_object()) {
    if (error != nullptr) *error = "camera raw: settings must be a json object";
    return false;
  }
  if (!reject_unknown_keys(root,
                           {"whiteBalance", "temperature", "tint", "exposure", "contrast",
                            "highlights", "shadows", "whites", "blacks", "vibrance", "saturation",
                            "texture", "clarity", "dehaze", "glow", "glowStyle", "glowRange",
                            "glowSpread", "glowWarmth", "vignetteAmount", "vignetteStyle",
                            "vignetteMidpoint", "vignetteRoundness", "vignetteFeather",
                            "vignetteHighlights", "grainAmount", "grainSize", "grainRoughness",
                            "curve", "mixer", "grading", "detail", "optics", "geometry",
                            "calibration"},
                           "", error)) {
    return false;
  }

  CameraRawSettings result;
  static const char* const kWhiteBalanceNames[] = {"Custom", "Auto"};
  static const char* const kGlowStyleNames[] = {"Diffusion", "Bloom", "Halation"};
  static const char* const kVignetteStyleNames[] = {"Highlight Priority", "Color Priority",
                                                   "Paint Overlay"};
  int white_balance = 0, glow_style = 0, vignette_style = 0;
  if (!choice_field(root, "whiteBalance", "", kWhiteBalanceNames, 2, &white_balance, error) ||
      !choice_field(root, "glowStyle", "", kGlowStyleNames, 3, &glow_style, error) ||
      !choice_field(root, "vignetteStyle", "", kVignetteStyleNames, 3, &vignette_style, error)) {
    return false;
  }
  result.white_balance = static_cast<CameraRawWhiteBalance>(white_balance);
  result.glow_style = static_cast<CameraRawGlowStyle>(glow_style);
  result.vignette_style = static_cast<CameraRawVignetteStyle>(vignette_style);

  if (!number_field(root, "temperature", "", -100.0, 100.0, &result.temperature, error) ||
      !number_field(root, "tint", "", -100.0, 100.0, &result.tint, error) ||
      !number_field(root, "exposure", "", -5.0, 5.0, &result.exposure, error) ||
      !number_field(root, "contrast", "", -100.0, 100.0, &result.contrast, error) ||
      !number_field(root, "highlights", "", -100.0, 100.0, &result.highlights, error) ||
      !number_field(root, "shadows", "", -100.0, 100.0, &result.shadows, error) ||
      !number_field(root, "whites", "", -100.0, 100.0, &result.whites, error) ||
      !number_field(root, "blacks", "", -100.0, 100.0, &result.blacks, error) ||
      !number_field(root, "vibrance", "", -100.0, 100.0, &result.vibrance, error) ||
      !number_field(root, "saturation", "", -100.0, 100.0, &result.saturation, error) ||
      !number_field(root, "texture", "", -100.0, 100.0, &result.texture, error) ||
      !number_field(root, "clarity", "", -100.0, 100.0, &result.clarity, error) ||
      !number_field(root, "dehaze", "", -100.0, 100.0, &result.dehaze, error) ||
      !number_field(root, "glow", "", 0.0, 100.0, &result.glow, error) ||
      !number_field(root, "glowRange", "", -100.0, 100.0, &result.glow_range, error) ||
      !number_field(root, "glowSpread", "", -100.0, 100.0, &result.glow_spread, error) ||
      !number_field(root, "glowWarmth", "", -100.0, 100.0, &result.glow_warmth, error) ||
      !number_field(root, "vignetteAmount", "", -100.0, 100.0, &result.vignette_amount, error) ||
      !number_field(root, "vignetteMidpoint", "", 0.0, 100.0, &result.vignette_midpoint, error) ||
      !number_field(root, "vignetteRoundness", "", -100.0, 100.0, &result.vignette_roundness,
                    error) ||
      !number_field(root, "vignetteFeather", "", 0.0, 100.0, &result.vignette_feather, error) ||
      !number_field(root, "vignetteHighlights", "", 0.0, 100.0, &result.vignette_highlights,
                    error) ||
      !number_field(root, "grainAmount", "", 0.0, 100.0, &result.grain_amount, error) ||
      !number_field(root, "grainSize", "", 0.0, 100.0, &result.grain_size, error) ||
      !number_field(root, "grainRoughness", "", 0.0, 100.0, &result.grain_roughness, error)) {
    return false;
  }

  const JsonValue* curve = nullptr;
  const JsonValue* mixer = nullptr;
  const JsonValue* grading = nullptr;
  const JsonValue* detail = nullptr;
  const JsonValue* optics = nullptr;
  const JsonValue* geometry = nullptr;
  const JsonValue* calibration = nullptr;
  if (!object_member(root, "curve", "", &curve, error) ||
      !object_member(root, "mixer", "", &mixer, error) ||
      !object_member(root, "grading", "", &grading, error) ||
      !object_member(root, "detail", "", &detail, error) ||
      !object_member(root, "optics", "", &optics, error) ||
      !object_member(root, "geometry", "", &geometry, error) ||
      !object_member(root, "calibration", "", &calibration, error)) {
    return false;
  }
  if ((curve != nullptr && !parse_curve(*curve, "curve", &result.curve, error)) ||
      (mixer != nullptr && !parse_mixer(*mixer, "mixer", &result.mixer, error)) ||
      (grading != nullptr && !parse_grading(*grading, "grading", &result.grading, error)) ||
      (detail != nullptr && !parse_detail(*detail, "detail", &result.detail, error)) ||
      (optics != nullptr && !parse_optics(*optics, "optics", &result.optics, error)) ||
      (geometry != nullptr && !parse_geometry(*geometry, "geometry", &result.geometry, error)) ||
      (calibration != nullptr &&
       !parse_calibration(*calibration, "calibration", &result.calibration, error))) {
    return false;
  }

  result.normalize();
  *settings = std::move(result);
  if (error != nullptr) error->clear();
  return true;
}

/* ============================================================ stage 装配 */

std::vector<CameraRawStage> camera_raw_stages_for(const CameraRawSettings& settings,
                                                  const CameraRawTableSet& tables,
                                                  const CameraRawBrightness& brightness) {
  std::vector<CameraRawStage> stages;
  const CameraRawCalibrationSettings& calibration = settings.calibration;
  const double primary[6] = {calibration.red_hue,     calibration.red_saturation,
                             calibration.green_hue,   calibration.green_saturation,
                             calibration.blue_hue,    calibration.blue_saturation};
  for (int index = 0; index < 6; ++index) {
    if (primary[index] != 0.0) {
      stages.push_back(
          camera_raw_fifty_stage(tables, kCameraRawCalibrationStart + index * 5, primary[index]));
    }
  }
  if (settings.exposure != 0.0) {
    stages.push_back(camera_raw_table_stage(tables, kCameraRawExposureStart, settings.exposure,
                                            -5.0, 0.5, 21));
  }
  if (settings.temperature != 0.0 || settings.tint != 0.0) {
    stages.push_back(
        camera_raw_white_balance_stage(tables, settings.temperature, settings.tint));
  }
  if (settings.contrast != 0.0) {
    stages.push_back(camera_raw_adaptive_stage(tables, kCameraRawContrastStart, settings.contrast,
                                               brightness.brightest));
  }
  if (settings.highlights != 0.0) {
    stages.push_back(camera_raw_adaptive_stage(tables, kCameraRawHighlightsStart,
                                               settings.highlights, brightness.log_brightest));
  }
  if (settings.shadows != 0.0) {
    stages.push_back(camera_raw_adaptive_stage(tables, kCameraRawShadowsStart, settings.shadows,
                                               brightness.luminance));
  }
  if (settings.whites != 0.0) {
    stages.push_back(
        camera_raw_table_stage(tables, kCameraRawWhitesStart, settings.whites, -100.0, 25.0, 9));
  }
  if (settings.blacks != 0.0) {
    stages.push_back(
        camera_raw_table_stage(tables, kCameraRawBlacksStart, settings.blacks, -100.0, 25.0, 9));
  }
  if (settings.saturation != 0.0) {
    stages.push_back(camera_raw_table_stage(tables, kCameraRawSaturationStart, settings.saturation,
                                            -100.0, 25.0, 9));
  }
  if (settings.vibrance != 0.0) {
    stages.push_back(camera_raw_table_stage(tables, kCameraRawVibranceStart, settings.vibrance,
                                            -100.0, 25.0, 9));
  }
  const CameraRawCurveSettings& curve = settings.curve;
  if (curve.has_measured_dividers()) {
    const double parametric[4] = {curve.shadows, curve.darks, curve.lights, curve.highlights};
    for (int index = 0; index < 4; ++index) {
      if (parametric[index] != 0.0) {
        stages.push_back(camera_raw_fifty_stage(
            tables, kCameraRawParametricStart + index * 5, parametric[index]));
      }
    }
  }
  return stages;
}

std::vector<CameraRawStage> camera_raw_mixer_stages(const CameraRawMixerSettings& mixer,
                                                    const CameraRawTableSet& tables) {
  std::vector<CameraRawStage> stages;
  const std::array<double, 8>* groups[3] = {&mixer.luminance, &mixer.saturation, &mixer.hue};
  for (int kind = 0; kind < 3; ++kind) {
    for (int color = 0; color < 8; ++color) {
      const double value = (*groups[kind])[static_cast<size_t>(color)];
      if (value != 0.0) {
        stages.push_back(
            camera_raw_fifty_stage(tables, kCameraRawMixerStart + (kind * 8 + color) * 5, value));
      }
    }
  }
  return stages;
}

std::vector<CameraRawStage> camera_raw_grading_stages(const CameraRawGradingSettings& grading,
                                                      const CameraRawTableSet& tables) {
  std::vector<CameraRawStage> stages;
  const CameraRawGradeWheel wheels[4] = {grading.shadows, grading.midtones, grading.highlights,
                                         grading.global};
  for (int index = 0; index < 4; ++index) {
    if (wheels[index].luminance != 0.0) {
      stages.push_back(camera_raw_fifty_stage(tables,
                                              kCameraRawGradingLuminanceStart + index * 5,
                                              wheels[index].luminance));
    }
  }

  const double balance = grading.balance / 100.0;
  const double highlight_share =
      std::max(0.0, balance < 0.0 ? 1.0 + 1.4 * balance : 1.0 + 2.0 * balance);
  const double shadow_share =
      std::max(0.0, balance < 0.0 ? 1.0 - 1.2 * balance : 1.0 - 1.4 * balance);
  const double blend_points[3] = {0.0, 50.0, 100.0};
  int blend_low = 0;
  double blend_up = 0.0;
  camera_raw_between(grading.blending, blend_points, 3, &blend_low, &blend_up);
  auto section = [](int blend_index, int wheel) {
    switch (blend_index) {
      case 0:
        return kCameraRawGradingStart + wheel * 36;
      case 1:
        return kCameraRawGradingStart + 72 + wheel * 36;
      default:
        return kCameraRawGradingStart + 216 + wheel * 36;
    }
  };

  struct Entry {
    int wheel;
    CameraRawGradeWheel color;
    double share;
    bool blends;
  };
  const Entry entries[4] = {{3, grading.global, 1.0, false},
                            {2, grading.midtones, 1.0, false},
                            {1, grading.highlights, highlight_share, true},
                            {0, grading.shadows, shadow_share, true}};
  const double levels[4] = {0.0, 25.0, 50.0, 100.0};
  for (const Entry& entry : entries) {
    if (!(entry.color.saturation * entry.share > 0.0)) continue;
    const double position = std::fmod(entry.color.hue, 360.0) / 30.0;
    const int first = static_cast<int>(position) % 12;
    const int second = (first + 1) % 12;
    const double around = position - std::floor(position);
    int level = 0;
    double up = 0.0;
    camera_raw_between(entry.color.saturation * entry.share, levels, 4, &level, &up);

    int blend_indices[2] = {1, 1};
    double blend_weights[2] = {1.0, 0.0};
    int blend_count = 1;
    if (entry.blends) {
      blend_indices[0] = blend_low;
      blend_indices[1] = blend_low + 1;
      blend_weights[0] = 1.0 - blend_up;
      blend_weights[1] = blend_up;
      blend_count = 2;
    }

    CameraRawStagePart parts[8];
    int count = 0;
    for (int blend = 0; blend < blend_count; ++blend) {
      const int base = section(blend_indices[blend], entry.wheel);
      const int hues[2] = {first, second};
      const double hue_weights[2] = {1.0 - around, around};
      for (int h = 0; h < 2; ++h) {
        const int saturations[2] = {level, level + 1};
        const double saturation_weights[2] = {1.0 - up, up};
        for (int s = 0; s < 2; ++s) {
          parts[count].table =
              saturations[s] == 0
                  ? nullptr
                  : tables.table(base + hues[h] * 3 + saturations[s] - 1);
          parts[count].weight =
              blend_weights[blend] * hue_weights[h] * saturation_weights[s];
          ++count;
        }
      }
    }
    stages.push_back(camera_raw_make_stage(parts, count));
  }
  return stages;
}

std::vector<float> camera_raw_folding_color(const CameraRawSettings& settings,
                                            const CameraRawTableSet& tables,
                                            std::vector<float> composed) {
  std::vector<float> table =
      composed.empty() ? camera_raw_identity_table() : std::move(composed);
  const CameraRawCurveSettings& curve = settings.curve;
  if (curve.adjusts()) {
    const CurveTables luts = curve_tables(curve);
    camera_raw_apply_table_curves(&table, luts.tone.data(), luts.red.data(), luts.green.data(),
                                  luts.blue.data());
  }
  std::vector<CameraRawStage> stages = camera_raw_mixer_stages(settings.mixer, tables);
  std::vector<CameraRawStage> grading = camera_raw_grading_stages(settings.grading, tables);
  stages.insert(stages.end(), grading.begin(), grading.end());
  if (!stages.empty()) {
    camera_raw_compose_onto(&table, stages.data(), static_cast<int>(stages.size()));
  }
  return table;
}

void camera_raw_apply_curve_color(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                                  const CameraRawSettings& settings,
                                  const CameraRawTableSet& tables) {
  if (rgba == nullptr || width <= 0 || height <= 0) return;
  const CameraRawCurveSettings& curve = settings.curve;
  const CameraRawMixerSettings& mixer = settings.mixer;
  const CameraRawGradingSettings& grading = settings.grading;
  const CurveTables luts = curve_tables(curve);
  /* 八个色族的公式系数留 0：它们的量由实测表承担（上游同款）。 */
  std::array<float, 24> zero_mixer{};
  const std::vector<float> point_floats = mixer.point_floats();
  const std::array<float, 12> grade{};

  const std::vector<CameraRawStage> mixer_stages = camera_raw_mixer_stages(mixer, tables);
  const std::vector<float> mixer_table =
      camera_raw_compose_stages(mixer_stages.data(), static_cast<int>(mixer_stages.size()));
  const std::vector<CameraRawStage> grading_stages =
      camera_raw_grading_stages(grading, tables);
  const std::vector<float> grade_table =
      camera_raw_compose_stages(grading_stages.data(), static_cast<int>(grading_stages.size()));

  ::adjust_camera_raw_curve_color(
      rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride, luts.tone.data(),
      luts.red.data(), luts.green.data(), luts.blue.data(), curve.refine_saturation / 100.0,
      zero_mixer.data(), static_cast<int>(point_floats.empty() ? 0 : mixer.points.size()),
      point_floats.data(), grade.data(), grading.blending / 100.0, grading.balance / 100.0, -1,
      mixer_table.empty() ? nullptr : mixer_table.data(),
      grade_table.empty() ? nullptr : grade_table.data(), kCameraRawGrid);
}

bool camera_raw_needs_tables(const CameraRawSettings& settings) {
  return !settings.is_identity();
}

/* ============================================================ 管线 */

comp_result camera_raw_apply_pixels(uint8_t* rgba, size_t stride, int32_t width, int32_t height,
                                    const CameraRawSettings& settings,
                                    const CameraRawTableSet* tables, double scale, uint32_t seed) {
  if (rgba == nullptr || width <= 0 || height <= 0) return COMP_ERR_INVALID_ARGUMENT;
  CameraRawSettings normalized = settings;
  normalized.normalize();
  if (normalized.is_identity()) return COMP_OK;
  if (normalized.geometry.adjusts()) return COMP_ERR_UNSUPPORTED;
  if (tables == nullptr || !tables->valid()) return COMP_ERR_STATE;

  const double pixel_scale = scale > 0.0 ? scale : 1.0;
  /* 上游 CameraRaw.apply:217-222；预览参数（clipping / mask / visualize）恒为关。 */
  const bool folds_color =
      normalized.mixer.points.empty() && normalized.curve.refine_saturation == 0.0;
  const bool color_adjusts =
      normalized.curve.adjusts() || normalized.mixer.adjusts() || normalized.grading.adjusts();
  const bool paint_color = !folds_color && color_adjusts;
  const bool paint_effects = normalized.adjusts_effects();
  const bool paint_detail_optics =
      normalized.detail.adjusts() || normalized.optics.adjusts();

  const bool adaptive = normalized.contrast != 0.0 || normalized.highlights != 0.0 ||
                        normalized.shadows != 0.0;
  CameraRawBrightness brightness;
  if (adaptive) {
    double out[3] = {0.0, 0.0, 0.0};
    ::camera_raw_statistics(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
                            out);
    brightness.brightest = out[0] * 255.0;
    brightness.luminance = out[1] * 255.0;
    brightness.log_brightest = out[2] * 255.0;
  }

  /* 校准先于主表：只画 Shadow Tint，六个主色走实测表（上游 applyCalibration）。 */
  if (normalized.calibration.adjusts() && normalized.calibration.shadow_tint != 0.0) {
    ::adjust_camera_raw_calibration(
        rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
        normalized.calibration.shadow_tint, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        static_cast<int>(normalized.calibration.process));
  }

  std::vector<CameraRawStage> stages =
      camera_raw_stages_for(normalized, *tables, brightness);
  std::vector<float> table =
      camera_raw_compose_stages(stages.data(), static_cast<int>(stages.size()));
  if (folds_color && color_adjusts) {
    table = camera_raw_folding_color(normalized, *tables, std::move(table));
  }
  if (!table.empty()) {
    ::adjust_camera_raw(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
                        table.data(), kCameraRawGrid, 0);
  }
  if (paint_color) {
    camera_raw_apply_curve_color(rgba, stride, width, height, normalized, *tables);
  }

  if (paint_effects) {
    if (normalized.texture != 0.0 || normalized.clarity != 0.0 || normalized.dehaze != 0.0 ||
        normalized.glow != 0.0 || normalized.vignette_amount != 0.0) {
      ::adjust_camera_raw_effects(
          rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
          normalized.texture, normalized.clarity, normalized.dehaze, normalized.glow,
          static_cast<int>(normalized.glow_style), normalized.glow_range,
          normalized.glow_spread, normalized.glow_warmth, normalized.vignette_amount,
          normalized.vignette_midpoint, normalized.vignette_roundness,
          normalized.vignette_feather, normalized.vignette_highlights,
          static_cast<int>(normalized.vignette_style), pixel_scale);
    }
    if (normalized.grain_amount > 0.0) {
      ::adjust_grain(rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
                     normalized.grain_amount, normalized.grain_kernel_size(),
                     normalized.grain_roughness, seed, 0.0, 0.0, 1.0 / pixel_scale);
    }
  }

  if (paint_detail_optics) {
    const CameraRawOpticsSettings& optics = normalized.optics;
    if (optics.adjusts()) {
      const uint8_t* exposure = tables->exposure_tables();
      if (exposure != nullptr) {
        ::adjust_camera_raw_optics(
            rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
            optics.remove_chromatic_aberration ? 1 : 0, optics.enable_lens_profile ? 1 : 0,
            optics.profile_distortion, optics.profile_vignetting,
            optics.distortion_k(kCameraRawLensStrength), optics.purple_amount,
            optics.purple_hue_low, optics.purple_hue_high, optics.green_amount,
            optics.green_hue_low, optics.green_hue_high, optics.vignette_amount,
            optics.vignette_midpoint, pixel_scale, exposure, kCameraRawSize);
      }
    }
    const CameraRawDetailSettings& detail = normalized.detail;
    if (detail.adjusts()) {
      ::adjust_camera_raw_detail(
          rgba, static_cast<size_t>(width), static_cast<size_t>(height), stride,
          detail.sharpen_amount, detail.sharpen_radius, detail.sharpen_detail,
          detail.sharpen_masking, detail.noise_luminance, detail.noise_luminance_detail,
          detail.noise_luminance_contrast, detail.noise_color, detail.noise_color_detail,
          detail.noise_color_smoothness, pixel_scale);
    }
  }
  return COMP_OK;
}

/* ============================================================ 文档入口 */

bool camera_raw_apply(Document* document, comp_layer_id target, const CameraRawSettings& settings,
                      double scale, uint32_t seed, comp_result* out_code, bool* out_changed,
                      std::string* note, std::string* error) {
  auto fail = [&](comp_result code, const std::string& message) {
    if (error != nullptr) *error = message;
    if (out_code != nullptr) *out_code = code;
    if (out_changed != nullptr) *out_changed = false;
    return false;
  };
  if (document == nullptr) return fail(COMP_ERR_INVALID_ARGUMENT, "camera raw needs a document");
  if (std::isnan(scale) || std::isinf(scale)) {
    return fail(COMP_ERR_INVALID_ARGUMENT, "camera raw scale must be finite");
  }

  CameraRawSettings normalized = settings;
  normalized.normalize();
  if (normalized.is_identity()) {
    if (out_code != nullptr) *out_code = COMP_OK;
    if (out_changed != nullptr) *out_changed = false;
    if (note != nullptr) *note += "camera raw settings are the identity; layer left unchanged";
    return true;
  }
  if (normalized.geometry.adjusts()) {
    return fail(COMP_ERR_UNSUPPORTED,
                "camera raw geometry (upright / projection / transform) is not supported");
  }
  const CameraRawTableSet* tables =
      document->engine() != nullptr ? document->engine()->camera_raw_tables() : nullptr;
  if (tables == nullptr || !tables->valid()) {
    return fail(COMP_ERR_STATE,
                "camera raw tables are not loaded; call comp_engine_load_camera_raw_tables first");
  }

  const bool owns_txn = !document->txn_active();
  std::string txn_error;
  if (owns_txn && document->txn_begin("Camera Raw", &txn_error) != COMP_OK) {
    return fail(COMP_ERR_STATE, txn_error);
  }
  auto abort_txn = [&]() {
    if (owns_txn) document->txn_abort(&txn_error);
  };
  Layer* layer = document->find(target);
  if (layer == nullptr) {
    abort_txn();
    return fail(COMP_ERR_NOT_FOUND, "camera raw target layer was not found");
  }
  /*
   * 事务已打开：像素管线要分配合成网格 / 工作副本，bad_alloc 会以异常穿出。
   * 这里先回滚再重抛，否则 ABI 的 catch 返回后文档会留下一个永不提交的活动事务。
   */
  comp_result status = COMP_OK;
  std::string failure;
  try {
    /* 8 位直接画；16 位先落 8 位工作副本（与工具层同一约定）；HDR 等拒绝。 */
    if (layer->pixels.format == COMP_PIXEL_RGBA16) {
      if (!deep_to_rgba8(layer->pixels, &layer->pixels)) {
        status = COMP_ERR_OUT_OF_MEMORY;
        failure = "camera raw could not build the 8-bit working copy";
      }
    } else if (layer->pixels.format != COMP_PIXEL_RGBA8) {
      status = COMP_ERR_STATE;
      failure = "camera raw needs an 8-bit or 16-bit layer";
    }
    if (status == COMP_OK) {
      std::vector<uint8_t>& pixels = layer->pixels.writable_pixels();
      status = camera_raw_apply_pixels(pixels.data(), layer->pixels.stride(),
                                       layer->pixels.width, layer->pixels.height, normalized,
                                       tables, scale, seed);
      if (status != COMP_OK) {
        failure = status == COMP_ERR_UNSUPPORTED ? "camera raw geometry is not supported"
                                                 : "camera raw could not apply the settings";
      }
    }
  } catch (...) {
    abort_txn();
    throw;
  }
  if (status != COMP_OK) {
    abort_txn();
    return fail(status, failure);
  }
  if (owns_txn) {
    std::string commit_error;
    if (document->txn_commit(&commit_error) != COMP_OK) {
      return fail(COMP_ERR_STATE, commit_error);
    }
  }
  document->mark_dirty(true);
  if (out_code != nullptr) *out_code = COMP_OK;
  if (out_changed != nullptr) *out_changed = true;
  if (note != nullptr) {
    *note += "camera raw applied";
    if (!normalized.mixer.points.empty() || normalized.curve.refine_saturation != 0.0) {
      *note += " with the per-pixel curve pass";
    } else if (normalized.curve.adjusts() || normalized.mixer.adjusts() ||
               normalized.grading.adjusts()) {
      *note += " with curve, mixer and grading folded into one lookup";
    }
  }
  return true;
}

}  // namespace compositor

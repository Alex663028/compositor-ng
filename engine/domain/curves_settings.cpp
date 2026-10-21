// AI生成
/*
 * curves_settings.cpp — Curves 控制点的读写与结构校验（键名/量程与上游 Curves.swift 一致）。
 */
#include "domain/curves_settings.h"

#include <algorithm>
#include <cmath>

#include "format/comp_json.h"

namespace compositor {
namespace {

/* 一个控制点：上游 Codable 的 {"x":N,"y":N}，或紧凑的 [x, y]。 */
bool read_point(const JsonValue& node, double* out_x, double* out_y) {
  if (node.is_object()) {
    const JsonValue* x = node.find("x");
    const JsonValue* y = node.find("y");
    if (x == nullptr || y == nullptr || !x->is_number() || !y->is_number()) return false;
    *out_x = x->as_number();
    *out_y = y->as_number();
    return true;
  }
  if (!node.is_array()) return false;
  const JsonValue::Array& pair = node.as_array();
  if (pair.size() < 2 || !pair[0].is_number() || !pair[1].is_number()) return false;
  *out_x = pair[0].as_number();
  *out_y = pair[1].as_number();
  return true;
}

/* settings 里的 curves.channels；缺失或类型不符返回 nullptr。 */
const JsonValue* channels_of(const JsonValue& root) {
  const JsonValue* curves = root.find("curves");
  if (curves == nullptr || !curves->is_object()) return nullptr;
  const JsonValue* channels = curves->find("channels");
  if (channels == nullptr || !channels->is_array()) return nullptr;
  return channels;
}

JsonValue point_to_json(const std::pair<double, double>& point) {
  JsonValue node = JsonValue::object({});
  node.set("x", JsonValue::number(point.first));
  node.set("y", JsonValue::number(point.second));
  return node;
}

/* settings 文本 → 根对象；空串按 "{}"（与 adjustment_table 同口径）。 */
JsonValue parse_root(const std::string& settings_json, std::string* error) {
  std::string parse_error;
  const JsonValue root =
      json_parse(settings_json.empty() ? std::string("{}") : settings_json, &parse_error);
  if (root.is_null() && !parse_error.empty()) {
    if (error != nullptr) *error = "invalid settings json: " + parse_error;
    return JsonValue();
  }
  if (!root.is_object()) {
    if (error != nullptr) *error = "adjustment settings must be a json object";
    return JsonValue();
  }
  return root;
}

}  // namespace

std::vector<std::pair<double, double>> curves_read_points(const std::string& settings_json,
                                                          uint32_t channel) {
  std::vector<std::pair<double, double>> points;
  if (channel >= kCurveChannelCount) return points;

  std::string parse_error;
  const JsonValue root =
      json_parse(settings_json.empty() ? std::string("{}") : settings_json, &parse_error);
  if (!root.is_object()) return points;
  const JsonValue* channels = channels_of(root);
  if (channels == nullptr) return points;
  const JsonValue::Array& arr = channels->as_array();
  if (channel >= arr.size() || !arr[channel].is_array()) return points;

  for (const JsonValue& node : arr[channel].as_array()) {
    double x = 0.0;
    double y = 0.0;
    if (!read_point(node, &x, &y)) continue;
    points.emplace_back(x, y);
  }
  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end(),
                           [](const std::pair<double, double>& a,
                              const std::pair<double, double>& b) { return a.first == b.first; }),
               points.end());
  return points;
}

bool curves_set_points(const std::string& settings_json, uint32_t channel,
                       const std::vector<std::pair<double, double>>& points, std::string* out_json,
                       std::string* error) {
  if (channel >= kCurveChannelCount) {
    if (error != nullptr) *error = "curve channel out of range";
    return false;
  }

  /* 空 = 复位成恒等曲线；数组下标有语义，不能把某一通道整个删掉。 */
  std::vector<std::pair<double, double>> normalized = points;
  if (normalized.empty()) normalized = {{0.0, 0.0}, {255.0, 255.0}};
  if (normalized.size() < kCurveMinPoints || normalized.size() > kCurveMaxPoints) {
    if (error != nullptr) *error = "curve channel needs 2..32 points";
    return false;
  }
  std::sort(normalized.begin(), normalized.end());
  for (size_t i = 0; i < normalized.size(); ++i) {
    const double x = normalized[i].first;
    const double y = normalized[i].second;
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 255.0 || y < 0.0 || y > 255.0) {
      if (error != nullptr) *error = "curve point out of range [0,255]";
      return false;
    }
    if (i > 0 && !(normalized[i - 1].first < x)) {
      if (error != nullptr) *error = "curve points must have distinct x";
      return false;
    }
  }
  if (normalized.front().first != 0.0 || normalized.back().first != 255.0) {
    if (error != nullptr) *error = "curve must start at x=0 and end at x=255";
    return false;
  }

  JsonValue root = parse_root(settings_json, error);
  if (root.is_null()) return false;

  /* curves 的其余键（如 channel）原样保留，只替换 channels。 */
  JsonValue curves = JsonValue::object({});
  const JsonValue* existing_curves = root.find("curves");
  if (existing_curves != nullptr && existing_curves->is_object()) curves = *existing_curves;
  JsonValue channels = JsonValue::array({});
  const JsonValue* existing_channels = curves.find("channels");
  if (existing_channels != nullptr && existing_channels->is_array()) channels = *existing_channels;

  JsonValue::Array list;
  list.reserve(normalized.size());
  for (const std::pair<double, double>& point : normalized) list.push_back(point_to_json(point));
  channels.set_index(channel, JsonValue::array(std::move(list)));
  curves.set("channels", std::move(channels));
  root.set("curves", std::move(curves));

  const std::string dumped = json_dump(root, 0);
  /* 自检：构造结果必须能通过同一套校验，避免面板造出引擎拒收的 settings。 */
  if (!curves_validate_settings(dumped, error)) return false;
  if (out_json != nullptr) *out_json = dumped;
  return true;
}

bool curves_validate_settings(const std::string& settings_json, std::string* error) {
  const JsonValue root = parse_root(settings_json, error);
  if (root.is_null()) return false;

  const JsonValue* curves = root.find("curves");
  if (curves == nullptr || curves->is_null()) return true;
  if (!curves->is_object()) {
    if (error != nullptr) *error = "curves must be a json object";
    return false;
  }

  const JsonValue* channels = curves->find("channels");
  if (channels == nullptr || channels->is_null()) return true;
  if (!channels->is_array()) {
    if (error != nullptr) *error = "curves.channels must be an array";
    return false;
  }

  const JsonValue::Array& arr = channels->as_array();
  if (arr.size() > kCurveChannelCount) {
    if (error != nullptr) *error = "curves.channels has more than 4 channels";
    return false;
  }
  for (size_t c = 0; c < arr.size(); ++c) {
    const JsonValue& entry = arr[c];
    if (entry.is_null()) continue; /* 缺省通道 = 恒等 */
    if (!entry.is_array()) {
      if (error != nullptr) *error = "curve channel must be an array of points";
      return false;
    }
    const JsonValue::Array& points = entry.as_array();
    if (points.empty()) continue; /* 空数组 = 恒等（求值侧同口径） */
    if (points.size() < kCurveMinPoints || points.size() > kCurveMaxPoints) {
      if (error != nullptr) *error = "curve channel needs 2..32 points";
      return false;
    }
    double previous = -1.0;
    for (size_t i = 0; i < points.size(); ++i) {
      double x = 0.0;
      double y = 0.0;
      if (!read_point(points[i], &x, &y)) {
        if (error != nullptr) *error = "curve point must be {x,y} or [x,y]";
        return false;
      }
      if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 255.0 || y < 0.0 ||
          y > 255.0) {
        if (error != nullptr) *error = "curve point out of range [0,255]";
        return false;
      }
      if (!(previous < x)) {
        if (error != nullptr) *error = "curve points must be sorted by strictly increasing x";
        return false;
      }
      previous = x;
      if (i == 0 && x != 0.0) {
        if (error != nullptr) *error = "curve must start at x=0";
        return false;
      }
      if (i + 1 == points.size() && x != 255.0) {
        if (error != nullptr) *error = "curve must end at x=255";
        return false;
      }
    }
  }
  return true;
}

}  // namespace compositor

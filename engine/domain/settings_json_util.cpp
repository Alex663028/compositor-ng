// AI生成
/* settings_json_util.cpp — 见头文件的设计说明。实现逐字搬自 adjustment_table.cpp。 */
#include "domain/settings_json_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace compositor {
namespace settings_json {

bool segment_index(const std::string& text, size_t* out) {
  if (text.empty() || text.size() > 6) return false;
  size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<size_t>(c - '0');
  }
  *out = value;
  return true;
}

const JsonValue* locate_leaf(const JsonValue& root, const char* path, bool* malformed) {
  const JsonValue* current = &root;
  const char* cursor = path;
  while (true) {
    const char* dot = std::strchr(cursor, '.');
    const size_t length = dot != nullptr ? static_cast<size_t>(dot - cursor) : std::strlen(cursor);
    const std::string key(cursor, length);
    size_t slot = 0;
    if (segment_index(key, &slot)) {
      if (!current->is_array()) {
        *malformed = true;
        return nullptr;
      }
      if (slot >= current->as_array().size()) return nullptr;
      current = &current->as_array()[slot];
    } else {
      if (!current->is_object()) {
        *malformed = true;
        return nullptr;
      }
      const JsonValue* next = current->find(key);
      if (next == nullptr) return nullptr;
      current = next;
    }
    if (dot == nullptr) return current;
    cursor = dot + 1;
  }
}

std::string number_to_text(double value) {
  char buffer[32];
  const bool integral = std::fabs(value - std::floor(value)) < 1e-9;
  if (integral) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%g", value);
  }
  return std::string(buffer);
}

std::vector<std::string> split_path(const char* path) {
  std::vector<std::string> segments;
  const std::string text(path);
  size_t start = 0;
  while (start <= text.size()) {
    const size_t dot = text.find('.', start);
    segments.push_back(text.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  return segments;
}

JsonValue set_path(const JsonValue& node, const std::vector<std::string>& segments, size_t index,
                   JsonValue value) {
  size_t slot = 0;
  if (segment_index(segments[index], &slot)) {
    JsonValue result = node.is_array() ? node : JsonValue::array({});
    if (index + 1 == segments.size()) {
      result.set_index(slot, std::move(value));
      return result;
    }
    const JsonValue* child =
        (node.is_array() && slot < node.as_array().size()) ? &node.as_array()[slot] : nullptr;
    const JsonValue next = child != nullptr ? *child : JsonValue();
    result.set_index(slot, set_path(next, segments, index + 1, std::move(value)));
    return result;
  }
  JsonValue result = node.is_object() ? node : JsonValue::object({});
  if (index + 1 == segments.size()) {
    result.set(segments[index], std::move(value));
    return result;
  }
  const JsonValue* child = node.is_object() ? node.find(segments[index]) : nullptr;
  const JsonValue next = child != nullptr ? *child : JsonValue::object({});
  result.set(segments[index], set_path(next, segments, index + 1, std::move(value)));
  return result;
}

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

}  // namespace settings_json
}  // namespace compositor

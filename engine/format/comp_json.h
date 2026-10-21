// AI生成
/*
 * comp_json.h — 极简 JSON 读写（供 .comp manifest 使用）。
 *
 * 不引入第三方依赖：M1 的 manifest 由本引擎自产自销，只需覆盖 JSON 标准子集
 * （对象/数组/字符串/数值/布尔/null）。M2 若需要更严格的合规性，
 * 可整体替换为 nlohmann/json 而不影响上层（调用点仅本目录）。
 */
#ifndef COMPOSITOR_FORMAT_COMP_JSON_H
#define COMPOSITOR_FORMAT_COMP_JSON_H

#include <string>
#include <utility>
#include <vector>

namespace compositor {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };
  using Array = std::vector<JsonValue>;
  using Object = std::vector<std::pair<std::string, JsonValue>>;

  JsonValue() : type_(Type::Null) {}

  static JsonValue boolean(bool v);
  static JsonValue number(double v);
  static JsonValue integer(long long v);
  static JsonValue string(std::string v);
  static JsonValue array(Array v);
  static JsonValue object(Object v);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0.0) const;
  long long as_integer(long long fallback = 0) const;
  std::string as_string(const std::string& fallback = std::string()) const;

  const Array& as_array() const;
  const Object& as_object() const;

  /* 对象取字段；不是对象或字段不存在时返回 nullptr。 */
  const JsonValue* find(const std::string& key) const;
  /* 便捷读取（带类型回退）。 */
  std::string string_or(const std::string& key, const std::string& fallback) const;
  long long integer_or(const std::string& key, long long fallback) const;
  double number_or(const std::string& key, double fallback) const;
  bool bool_or(const std::string& key, bool fallback) const;

  void set(std::string key, JsonValue value);
  void push(JsonValue value);
  /* 数组下标赋值：非数组先转数组，缺位补 null（保持值语义，不影响既有元素）。 */
  void set_index(size_t index, JsonValue value);

 private:
  Type type_;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

/* 序列化：indent > 0 时按该空格数缩进换行；indent = 0 时输出紧凑形式。 */
std::string json_dump(const JsonValue& value, int indent = 2);

/* 解析：失败时返回 Null 并写 error。 */
JsonValue json_parse(const std::string& text, std::string* error);

}  // namespace compositor

#endif /* COMPOSITOR_FORMAT_COMP_JSON_H */

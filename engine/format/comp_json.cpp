// AI生成
#include "format/comp_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace compositor {

namespace {

const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;

void append_utf8(std::string* out, unsigned int cp) {
  if (cp <= 0x7F) {
    out->push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string escape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          /* UTF-8 原始字节直接输出，保证中文名称可读。 */
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string dump_number(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    std::ostringstream ss;
    ss << static_cast<long long>(v);
    return ss.str();
  }
  std::ostringstream ss;
  ss.precision(9);
  ss << v;
  return ss.str();
}

void dump_into(const JsonValue& v, int indent, int depth, std::string* out) {
  const bool pretty = indent > 0;
  const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * depth, ' ') : "";
  const std::string pad_inner =
      pretty ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : "";
  const char* nl = pretty ? "\n" : "";

  switch (v.type()) {
    case JsonValue::Type::Null:   *out += "null"; break;
    case JsonValue::Type::Bool:   *out += v.as_bool() ? "true" : "false"; break;
    case JsonValue::Type::Number: *out += dump_number(v.as_number()); break;
    case JsonValue::Type::String: *out += "\"" + escape(v.as_string()) + "\""; break;
    case JsonValue::Type::Array: {
      const JsonValue::Array& a = v.as_array();
      if (a.empty()) { *out += "[]"; break; }
      *out += "[";
      for (size_t i = 0; i < a.size(); ++i) {
        if (i != 0) *out += ",";
        *out += nl;
        *out += pad_inner;
        dump_into(a[i], indent, depth + 1, out);
      }
      *out += nl;
      *out += pad;
      *out += "]";
      break;
    }
    case JsonValue::Type::Object: {
      const JsonValue::Object& o = v.as_object();
      if (o.empty()) { *out += "{}"; break; }
      *out += "{";
      for (size_t i = 0; i < o.size(); ++i) {
        if (i != 0) *out += ",";
        *out += nl;
        *out += pad_inner;
        *out += "\"" + escape(o[i].first) + "\":";
        if (pretty) *out += " ";
        dump_into(o[i].second, indent, depth + 1, out);
      }
      *out += nl;
      *out += pad;
      *out += "}";
      break;
    }
  }
}

class Parser {
 public:
  Parser(const std::string& text, std::string* error) : text_(text), error_(error) {}

  bool parse(JsonValue* out) {
    skip_ws();
    if (!parse_value(out)) return false;
    skip_ws();
    if (pos_ != text_.size()) return fail("trailing characters");
    return true;
  }

 private:
  bool fail(const char* message) {
    if (error_ != nullptr) {
      std::ostringstream ss;
      ss << message << " at offset " << pos_;
      *error_ = ss.str();
    }
    return false;
  }

  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  /*
   * P2-9：递归下降解析器必须有深度上限。manifest 允许到 4 MiB，`[[[[[...` 这样的
   * 深嵌套足以把栈打爆（表现为段错误而非可解释错误）。128 层远超真实 manifest 的嵌套。
   */
  static constexpr int kMaxDepth = 128;

  bool parse_value(JsonValue* out) {
    if (depth_ >= kMaxDepth) return fail("nesting too deep");
    ++depth_;
    const bool ok = parse_value_inner(out);
    --depth_;
    return ok;
  }

  bool parse_value_inner(JsonValue* out) {
    if (pos_ >= text_.size()) return fail("unexpected end of input");
    switch (text_[pos_]) {
      case '{': return parse_object(out);
      case '[': return parse_array(out);
      case '"': {
        std::string s;
        if (!parse_string(&s)) return false;
        *out = JsonValue::string(std::move(s));
        return true;
      }
      case 't':
        if (text_.compare(pos_, 4, "true") == 0) { pos_ += 4; *out = JsonValue::boolean(true); return true; }
        return fail("invalid literal");
      case 'f':
        if (text_.compare(pos_, 5, "false") == 0) { pos_ += 5; *out = JsonValue::boolean(false); return true; }
        return fail("invalid literal");
      case 'n':
        if (text_.compare(pos_, 4, "null") == 0) { pos_ += 4; *out = JsonValue(); return true; }
        return fail("invalid literal");
      default:
        return parse_number(out);
    }
  }

  bool parse_number(JsonValue* out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    bool any = false;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        ++pos_;
        any = true;
      } else {
        break;
      }
    }
    if (!any) return fail("invalid number");
    const std::string token = text_.substr(start, pos_ - start);
    char* end = nullptr;
    const double value = std::strtod(token.c_str(), &end);
    if (end == nullptr || *end != '\0') return fail("invalid number");
    *out = JsonValue::number(value);
    return true;
  }

  bool parse_string(std::string* out) {
    if (pos_ >= text_.size() || text_[pos_] != '"') return fail("expected string");
    ++pos_;
    out->clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) return fail("unterminated escape");
      const char esc = text_[pos_++];
      switch (esc) {
        case '"':  out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/':  out->push_back('/'); break;
        case 'n':  out->push_back('\n'); break;
        case 'r':  out->push_back('\r'); break;
        case 't':  out->push_back('\t'); break;
        case 'b':  out->push_back('\b'); break;
        case 'f':  out->push_back('\f'); break;
        case 'u': {
          if (pos_ + 4 > text_.size()) return fail("bad \\u escape");
          unsigned int cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
            else return fail("bad hex in \\u escape");
          }
          append_utf8(out, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    return fail("unterminated string");
  }

  bool parse_array(JsonValue* out) {
    ++pos_; /* '[' */
    JsonValue::Array items;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      *out = JsonValue::array(std::move(items));
      return true;
    }
    while (true) {
      skip_ws();
      JsonValue item;
      if (!parse_value(&item)) return false;
      items.push_back(std::move(item));
      skip_ws();
      if (pos_ >= text_.size()) return fail("unterminated array");
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == ']') { ++pos_; break; }
      return fail("expected ',' or ']'");
    }
    *out = JsonValue::array(std::move(items));
    return true;
  }

  bool parse_object(JsonValue* out) {
    ++pos_; /* '{' */
    JsonValue::Object members;
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      *out = JsonValue::object(std::move(members));
      return true;
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(&key)) return false;
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != ':') return fail("expected ':'");
      ++pos_;
      skip_ws();
      JsonValue value;
      if (!parse_value(&value)) return false;
      members.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (pos_ >= text_.size()) return fail("unterminated object");
      if (text_[pos_] == ',') { ++pos_; continue; }
      if (text_[pos_] == '}') { ++pos_; break; }
      return fail("expected ',' or '}'");
    }
    *out = JsonValue::object(std::move(members));
    return true;
  }

  const std::string& text_;
  std::string* error_;
  size_t pos_ = 0;
  int depth_ = 0;
};

}  // namespace

JsonValue JsonValue::boolean(bool v) {
  JsonValue j;
  j.type_ = Type::Bool;
  j.bool_ = v;
  return j;
}

JsonValue JsonValue::number(double v) {
  JsonValue j;
  j.type_ = Type::Number;
  j.number_ = v;
  return j;
}

JsonValue JsonValue::integer(long long v) {
  return number(static_cast<double>(v));
}

JsonValue JsonValue::string(std::string v) {
  JsonValue j;
  j.type_ = Type::String;
  j.string_ = std::move(v);
  return j;
}

JsonValue JsonValue::array(Array v) {
  JsonValue j;
  j.type_ = Type::Array;
  j.array_ = std::move(v);
  return j;
}

JsonValue JsonValue::object(Object v) {
  JsonValue j;
  j.type_ = Type::Object;
  j.object_ = std::move(v);
  return j;
}

bool JsonValue::as_bool(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}

double JsonValue::as_number(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}

long long JsonValue::as_integer(long long fallback) const {
  return type_ == Type::Number ? static_cast<long long>(number_) : fallback;
}

std::string JsonValue::as_string(const std::string& fallback) const {
  return type_ == Type::String ? string_ : fallback;
}

const JsonValue::Array& JsonValue::as_array() const {
  return type_ == Type::Array ? array_ : kEmptyArray;
}

const JsonValue::Object& JsonValue::as_object() const {
  return type_ == Type::Object ? object_ : kEmptyObject;
}

const JsonValue* JsonValue::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& kv : object_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string JsonValue::string_or(const std::string& key, const std::string& fallback) const {
  const JsonValue* v = find(key);
  return v == nullptr ? fallback : v->as_string(fallback);
}

long long JsonValue::integer_or(const std::string& key, long long fallback) const {
  const JsonValue* v = find(key);
  return v == nullptr ? fallback : v->as_integer(fallback);
}

double JsonValue::number_or(const std::string& key, double fallback) const {
  const JsonValue* v = find(key);
  return v == nullptr ? fallback : v->as_number(fallback);
}

bool JsonValue::bool_or(const std::string& key, bool fallback) const {
  const JsonValue* v = find(key);
  return v == nullptr ? fallback : v->as_bool(fallback);
}

void JsonValue::set(std::string key, JsonValue value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  for (auto& kv : object_) {
    if (kv.first == key) {
      kv.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void JsonValue::push(JsonValue value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    array_.clear();
  }
  array_.push_back(std::move(value));
}

std::string json_dump(const JsonValue& value, int indent) {
  std::string out;
  dump_into(value, indent, 0, &out);
  return out;
}

JsonValue json_parse(const std::string& text, std::string* error) {
  Parser parser(text, error);
  JsonValue value;
  if (!parser.parse(&value)) return JsonValue();
  return value;
}

}  // namespace compositor

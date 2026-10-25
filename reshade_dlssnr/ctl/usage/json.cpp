#include "ctl/usage/json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reshade_dlssnr::json {

Value Value::MakeObject() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

Value Value::MakeArray() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

bool Value::as_bool(bool fallback) const {
  switch (type_) {
    case Type::Bool: return bool_;
    case Type::Int: return int_ != 0;
    case Type::Float: return float_ != 0.0f;
    case Type::String:
      if (string_ == "true" || string_ == "1") return true;
      if (string_ == "false" || string_ == "0") return false;
      return fallback;
    default: return fallback;
  }
}

int Value::as_int(int fallback) const {
  switch (type_) {
    case Type::Int: return int_;
    case Type::Float: return static_cast<int>(float_);
    case Type::Bool: return bool_ ? 1 : 0;
    case Type::String: {
      char* end = nullptr;
      const long parsed = std::strtol(string_.c_str(), &end, 10);
      if (end == string_.c_str()) return fallback;
      return static_cast<int>(parsed);
    }
    default: return fallback;
  }
}

float Value::as_float(float fallback) const {
  switch (type_) {
    case Type::Float: return float_;
    case Type::Int: return static_cast<float>(int_);
    case Type::Bool: return bool_ ? 1.0f : 0.0f;
    case Type::String: {
      char* end = nullptr;
      const float parsed = std::strtof(string_.c_str(), &end);
      if (end == string_.c_str()) return fallback;
      return parsed;
    }
    default: return fallback;
  }
}

std::string Value::as_string() const {
  switch (type_) {
    case Type::String: return string_;
    case Type::Int: return std::to_string(int_);
    case Type::Float: {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(float_));
      return buffer;
    }
    case Type::Bool: return bool_ ? "1" : "0";
    default: return {};
  }
}

Value& Value::operator[](const std::string& key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    object_.clear();
  }
  return object_[key];
}

bool Value::has(const std::string& key) const {
  return type_ == Type::Object && object_.find(key) != object_.end();
}

bool Value::erase(const std::string& key) {
  if (type_ != Type::Object) return false;
  return object_.erase(key) != 0;
}

Value* Value::find(const std::string& path) {
  if (path.empty()) return this;
  Value* current = this;
  size_t begin = 0;
  while (begin < path.size()) {
    size_t end = path.find('.', begin);
    if (end == std::string::npos) end = path.size();
    const std::string segment = path.substr(begin, end - begin);
    if (!current->is_object()) return nullptr;
    auto it = current->object_.find(segment);
    if (it == current->object_.end()) return nullptr;
    current = &it->second;
    begin = end + 1;
  }
  return current;
}

const Value* Value::find(const std::string& path) const {
  return const_cast<Value*>(this)->find(path);
}

Value& Value::ensure(const std::string& path) {
  if (path.empty()) return *this;
  Value* current = this;
  size_t begin = 0;
  while (begin < path.size()) {
    size_t end = path.find('.', begin);
    if (end == std::string::npos) end = path.size();
    const std::string segment = path.substr(begin, end - begin);
    if (!current->is_object()) {
      current->type_ = Type::Object;
      current->object_.clear();
    }
    current = &current->object_[segment];
    begin = end + 1;
  }
  return *current;
}

bool Value::erase_path(const std::string& path) {
  if (path.empty()) return false;
  const size_t last_dot = path.find_last_of('.');
  Value* parent = this;
  std::string key = path;
  if (last_dot != std::string::npos) {
    parent = find(path.substr(0, last_dot));
    key = path.substr(last_dot + 1);
  }
  if (parent == nullptr || !parent->is_object()) return false;
  auto it = parent->object_.find(key);
  if (it == parent->object_.end()) return false;
  parent->object_.erase(it);
  return true;
}

namespace {

void DumpString(std::string& out, const std::string& text) {
  out += '"';
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Value Parse(std::string* error) {
    SkipWhitespace();
    Value v = ParseValue();
    if (!error_.empty()) {
      if (error) *error = error_;
      return Value();
    }
    SkipWhitespace();
    if (pos_ < text_.size()) {
      if (error) *error = "trailing characters";
      return Value();
    }
    return v;
  }

 private:
  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++pos_; continue; }
      if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
        pos_ += 2;
        while (pos_ < text_.size() && text_[pos_] != '\n') ++pos_;
        continue;
      }
      break;
    }
  }

  bool Match(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
    return false;
  }

  bool MatchToken(const char* token) {
    const size_t len = std::strlen(token);
    if (pos_ + len > text_.size()) return false;
    if (text_.compare(pos_, len, token) != 0) return false;
    pos_ += len;
    return true;
  }

  Value ParseValue() {
    SkipWhitespace();
    if (pos_ >= text_.size()) { error_ = "unexpected end"; return Value(); }
    const char c = text_[pos_];
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') return Value(ParseString());
    if (c == 't') { return MatchToken("true") ? Value(true) : Error("bad token"); }
    if (c == 'f') { return MatchToken("false") ? Value(false) : Error("bad token"); }
    if (c == 'n') { return MatchToken("null") ? Value() : Error("bad token"); }
    return ParseNumber();
  }

  Value Error(const char* message) {
    error_ = message;
    return Value();
  }

  Value ParseObject() {
    if (!Match('{')) return Error("expected '{'");
    Value result = Value::MakeObject();
    SkipWhitespace();
    if (Match('}')) return result;
    while (true) {
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') return Error("expected key");
      const std::string key = ParseString();
      if (!error_.empty()) return Value();
      SkipWhitespace();
      if (!Match(':')) return Error("expected ':'");
      Value child = ParseValue();
      if (!error_.empty()) return Value();
      result[key] = std::move(child);
      SkipWhitespace();
      if (Match('}')) return result;
      if (!Match(',')) return Error("expected ',' or '}'");
    }
  }

  Value ParseArray() {
    if (!Match('[')) return Error("expected '['");
    Value result = Value::MakeArray();
    SkipWhitespace();
    if (Match(']')) return result;
    while (true) {
      Value child = ParseValue();
      if (!error_.empty()) return Value();
      result.array().push_back(std::move(child));
      SkipWhitespace();
      if (Match(']')) return result;
      if (!Match(',')) return Error("expected ',' or ']'");
    }
  }

  std::string ParseString() {
    if (!Match('"')) { error_ = "expected '\"'"; return {}; }
    std::string result;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return result;
      if (c != '\\') { result += c; continue; }
      if (pos_ >= text_.size()) { error_ = "bad escape"; return {}; }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case '/': result += '/'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (pos_ + 4 > text_.size()) { error_ = "bad unicode"; return {}; }
          const std::string hex = text_.substr(pos_, 4);
          pos_ += 4;
          unsigned code = 0;
          for (char h : hex) {
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else { error_ = "bad unicode"; return {}; }
          }
          if (code < 0x80) result += static_cast<char>(code);
          else if (code < 0x800) {
            result += static_cast<char>(0xC0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3F));
          } else {
            result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
          }
          break;
        }
        default: error_ = "bad escape"; return {};
      }
    }
    error_ = "unterminated string";
    return {};
  }

  Value ParseNumber() {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    bool is_float = false;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      is_float = true;
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      is_float = true;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    if (start == pos_) return Error("expected number");
    const std::string token = text_.substr(start, pos_ - start);
    try {
      if (is_float) return Value(std::stof(token));
      return Value(std::stoi(token));
    } catch (...) {
      return Error("bad number");
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

std::string Value::dump() const {
  std::string out;
  DumpTo(out, 0);
  out += '\n';
  return out;
}

void Value::DumpTo(std::string& out, int indent) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Int: out += std::to_string(int_); break;
    case Type::Float: {
      char buffer[64];
      std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(float_));
      out += buffer;
      break;
    }
    case Type::String: DumpString(out, string_); break;
    case Type::Object: {
      if (object_.empty()) { out += "{}"; break; }
      out += "{\n";
      const std::string child_indent(indent + 2, ' ');
      bool first = true;
      for (const auto& kv : object_) {
        if (!first) out += ",\n";
        first = false;
        out += child_indent;
        DumpString(out, kv.first);
        out += ": ";
        kv.second.DumpTo(out, indent + 2);
      }
      out += '\n';
      out += std::string(indent, ' ');
      out += '}';
      break;
    }
    case Type::Array: {
      if (array_.empty()) { out += "[]"; break; }
      out += "[\n";
      const std::string child_indent(indent + 2, ' ');
      bool first = true;
      for (const Value& v : array_) {
        if (!first) out += ",\n";
        first = false;
        out += child_indent;
        v.DumpTo(out, indent + 2);
      }
      out += '\n';
      out += std::string(indent, ' ');
      out += ']';
      break;
    }
  }
}

Value Value::parse(const std::string& text, std::string* error) {
  Parser parser(text);
  return parser.Parse(error);
}

}  // namespace reshade_dlssnr::json

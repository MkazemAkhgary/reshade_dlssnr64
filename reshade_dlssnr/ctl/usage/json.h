// Minimal JSON value, parser, and writer.
//
// Supports objects, arrays, strings, integers, floats, booleans, and null.
// Object keys are held in a std::map so the dumped form is deterministic
// (sorted keys); array order is preserved.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace reshade_dlssnr::json {

class Value {
 public:
  enum class Type { Null, Bool, Int, Float, String, Object, Array };

  Value() = default;
  explicit Value(bool v) : type_(Type::Bool), bool_(v) {}
  explicit Value(int v) : type_(Type::Int), int_(v) {}
  explicit Value(float v) : type_(Type::Float), float_(v) {}
  explicit Value(std::string v) : type_(Type::String), string_(std::move(v)) {}

  static Value MakeObject();
  static Value MakeArray();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }

  bool as_bool(bool fallback = false) const;
  int as_int(int fallback = 0) const;
  float as_float(float fallback = 0.0f) const;
  std::string as_string() const;

  Value& operator[](const std::string& key);
  bool has(const std::string& key) const;
  bool erase(const std::string& key);
  const std::map<std::string, Value>& object() const { return object_; }

  // Array access. A non-const access coerces the value to an array so a
  // first push_back works without a separate MakeArray step. Read-only
  // access returns an empty vector for non-arrays. Deliberately no
  // operator[](size_t): v[0] would be ambiguous with the string overload.
  std::vector<Value>& array() {
    if (type_ != Type::Array) {
      type_ = Type::Array;
      array_.clear();
    }
    return array_;
  }
  const std::vector<Value>& array() const {
    static const std::vector<Value> kEmpty;
    return type_ == Type::Array ? array_ : kEmpty;
  }
  size_t size() const {
    if (type_ == Type::Object) return object_.size();
    if (type_ == Type::Array) return array_.size();
    return 0;
  }

  Value* find(const std::string& path);
  const Value* find(const std::string& path) const;
  Value& ensure(const std::string& path);
  bool erase_path(const std::string& path);

  std::string dump() const;
  static Value parse(const std::string& text, std::string* error);

 private:
  void DumpTo(std::string& out, int indent) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  int int_ = 0;
  float float_ = 0.0f;
  std::string string_;
  std::map<std::string, Value> object_;
  std::vector<Value> array_;
};

}  // namespace reshade_dlssnr::json

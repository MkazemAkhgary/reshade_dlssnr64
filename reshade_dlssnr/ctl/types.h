// ctl value model: metadata, type-erased setting storage, and the setting
// interface every concrete setting type implements.
//
// Adding a new setting type is two steps:
//   1. derive from Setting and implement the pure-virtual surface below;
//   2. register a factory with the Registry.
// No existing type, the manager, or any call site needs to change.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace reshade_dlssnr {

inline std::string CStrToString(const char* text) {
  return text != nullptr ? std::string(text) : std::string();
}

namespace ctl {

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

// The storage a setting's value lives in. Values own their data so copies in
// the resolved cache, write path and snapshots remain valid.
struct Value {
  enum class Kind : uint8_t { Float, Int, Bool, Enum, String, IntList };

  Kind kind = Kind::Float;
  float as_float = 0.0f;
  int as_int = 0;
  bool as_bool = false;
  std::string as_string;
  std::vector<int64_t> as_int_list;

  Value() = default;
  static Value Float(float v) { Value x; x.kind = Kind::Float; x.as_float = v; return x; }
  static Value Int(int v) { Value x; x.kind = Kind::Int; x.as_int = v; return x; }
  static Value Bool(bool v) { Value x; x.kind = Kind::Bool; x.as_bool = v; return x; }
  static Value Enum(int v) { Value x; x.kind = Kind::Enum; x.as_int = v; return x; }
  static Value String(std::string v) {
    Value x;
    x.kind = Kind::String;
    x.as_string = std::move(v);
    return x;
  }
  static Value IntList(std::vector<int64_t> v) {
    Value x;
    x.kind = Kind::IntList;
    x.as_int_list = std::move(v);
    return x;
  }

  bool operator==(const Value& o) const;
  bool operator!=(const Value& o) const { return !(*this == o); }
};

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

// Immutable description of one setting. Extensible: new fields are appended,
// existing settings simply leave them at their defaults, so adding metadata
// never breaks a previously declared setting.
enum class EnumPresentation : uint8_t { Combo, GroupButtons };
enum class PersistenceTarget : uint8_t { Global, Profile };
enum class PersistenceBackend : uint8_t { Json, Ini };

struct SettingMeta {
  std::string name;      // stable identifier, unique within its section
  std::string label;     // user-facing row label
  std::string tooltip;
  // Category paths use '/' to describe nested subsections. The framework
  // treats each path component as a collapsible group.
  std::string category;
  bool persist = true;   // false = never written to config
  bool persist_in_profiles = true;
  PersistenceBackend backend = PersistenceBackend::Json;
  bool category_toggle = false;  // bool setting drawn in its category header
  std::string inline_toggle_setting;  // bool drawn before this setting's label
  EnumPresentation enum_presentation = EnumPresentation::Combo;
};

// Whether a row is interactive this frame. A setting's own rule, when set, is
// consulted before any section-wide rule. Declared here so a setting can carry
// its gate as metadata, exactly like its tooltip or range, instead of the
// section having to be told the setting's name afterwards.
//
// `section` is the section the row is being drawn in. The framework knows it
// and the predicate usually wants it -- the common gate is "another checkbox
// in this same section", and without it every such predicate would have to
// smuggle a copy of the name in through `context`.
class ValueStore;
class InstanceKey;
using EnablePredicate = bool (*)(const void* context, const ValueStore& store,
                                 const InstanceKey& key,
                                 const std::string& section);
using VisibilityPredicate = EnablePredicate;

// Predicate behind the EnableWhen(const char* setting_name) overload. The
// setting name is carried as `context`; the value is treated as a boolean:
// bool true, or non-zero int/enum/float.
bool BoolFromSetting(const void* context, const ValueStore& store,
                   const InstanceKey& key, const std::string& section);

// ---------------------------------------------------------------------------
// Setting
// ---------------------------------------------------------------------------

struct SettingContext;

// One declared setting. Concrete types live in builtin_settings.h and
// elsewhere; the core only ever sees this interface.
class Setting {
 public:
  virtual ~Setting() = default;

  const SettingMeta& meta() const { return meta_; }
  SettingMeta& meta_mutable() { return meta_; }

  // --- value interface ---------------------------------------------------

  // The declared default. Used by reset, and as the end of the resolution
  // chain when no instance in the parent chain overrides the setting.
  virtual Value DefaultValue() const = 0;

  // Constrain a value to the type's own rules (range, step, enum bounds).
  // Called after every load and after every programmatic write.
  virtual void Clamp(Value& value) const = 0;

  // Copy a value into `out`, converting between storage kinds if the type
  // supports it. Returns false if the value is not representable.
  virtual bool Assign(Value& out, const Value& in) const = 0;

  // Human-readable rendering of a value, for traces and diagnostics.
  virtual std::string ToString(const Value& value) const = 0;

  // Parse a value from its serialized text form. Returns false and leaves
  // `out` untouched when the text is not valid for this type.
  virtual bool FromString(const std::string& text, Value& out) const = 0;

  // Persist normally to global settings but exclude this setting from profiles.
  Setting& GlobalOnly() { meta_.persist_in_profiles = false; return *this; }

  Setting& Backend(PersistenceBackend backend) {
    meta_.backend = backend;
    return *this;
  }

  void SetInlineToggle(const char* bool_setting) {
    meta_.inline_toggle_setting = CStrToString(bool_setting);
  }

  // --- rendering ---------------------------------------------------------

  // Gate this row's interactivity, expressed as setting metadata so it sits
  // next to the tooltip and range at the declaration site and no caller needs
  // the setting's name. Overrides any section-wide gate; a null predicate
  // leaves the row enabled.
  //
  // Concrete types also expose a typed `EnableWhen(...)` returning their own
  // type, so it chains with .Tooltip/.Range in any order.
  void SetEnabled(EnablePredicate enabled, const void* context = nullptr) {
    enabled_ = enabled;
    enabled_context_ = context;
  }

  EnablePredicate enabled() const { return enabled_; }
  const void* enabled_context() const { return enabled_context_; }

  void SetVisible(VisibilityPredicate visible, const void* context = nullptr) {
    visible_ = visible;
    visible_context_ = context;
  }

  VisibilityPredicate visible() const { return visible_; }
  const void* visible_context() const { return visible_context_; }

  // Draw one row. `resolved` is what the user should see; `ctx` carries the
  // enabled flag. Returns true when the user changed the value.
  virtual bool Draw(const SettingContext& ctx, const Value& resolved,
                    Value& out) const = 0;

 protected:
  SettingMeta meta_;

 private:
  EnablePredicate enabled_ = nullptr;
  const void* enabled_context_ = nullptr;
  VisibilityPredicate visible_ = nullptr;
  const void* visible_context_ = nullptr;
};

// What a setting's Draw() needs in order to render a row.
struct SettingContext {
  const Setting* setting = nullptr;

  // False when an enable rule gated this row off this frame. The drawer
  // already wraps Draw() in a disabled scope, so a type that does nothing
  // with this still behaves correctly.
  bool enabled = true;
};


}  // namespace ctl
}  // namespace reshade_dlssnr

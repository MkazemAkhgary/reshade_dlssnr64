// ctl built-in setting types: float, int, bool, enum, string, integer list.
//
// Each is a dedicated class that owns its own storage, validation, text
// form, and UI row. They share the row layout helpers in ui/ui_widgets.h so
// a declarative row looks identical to a hand-written one.

#pragma once

#include <limits>
#include <string>
#include <vector>

#include "ctl/types.h"

namespace reshade_dlssnr {
namespace ctl {

// ---------------------------------------------------------------------------
// FloatSetting
// ---------------------------------------------------------------------------

class FloatSetting : public Setting {
 public:
  FloatSetting(const char* name, const char* label);
  FloatSetting(const char* name, const char* label, float default_value);

  FloatSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  FloatSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  FloatSetting& Category(const char* text) { meta_.category = CStrToString(text); return *this; }
  FloatSetting& EnableWhen(EnablePredicate enabled, const void* context = nullptr) {
    SetEnabled(enabled, context);
    return *this;
  }
  FloatSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }
  // Slider drag range. When ClampRange has not been set, this is also the
  // accepted input range.
  FloatSetting& Range(float lo, float hi) {
    ui_lo_ = lo;
    ui_hi_ = hi;
    if (!has_clamp_range_) {
      lo_ = lo;
      hi_ = hi;
    }
    return *this;
  }
  // Absolute bounds for typed input, programmatic writes, and loads. Takes
  // authority over Range: the slider is narrowed to fit.
  FloatSetting& ClampRange(float lo, float hi) {
    lo_ = lo;
    hi_ = hi;
    has_clamp_range_ = true;
    return *this;
  }
  FloatSetting& Format(const char* fmt) { format_ = CStrToString(fmt); return *this; }
  FloatSetting& NoSave() { meta_.persist = false; return *this; }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  float default_ = 0.0f;
  // Slider bounds. Unbounded by default so ClampRange alone decides the
  // slider until Range is called.
  float ui_lo_ = std::numeric_limits<float>::lowest();
  float ui_hi_ = std::numeric_limits<float>::max();
  // Accepted input bounds. Range populates these when ClampRange is absent.
  float lo_ = 0.0f;
  float hi_ = 1.0f;
  bool has_clamp_range_ = false;
  std::string format_ = "%.3f";
};

// ---------------------------------------------------------------------------
// IntSetting
// ---------------------------------------------------------------------------

class IntSetting : public Setting {
 public:
  IntSetting(const char* name, const char* label);
  IntSetting(const char* name, const char* label, int default_value);

  IntSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  IntSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  IntSetting& Category(const char* text) { meta_.category = CStrToString(text); return *this; }
  IntSetting& EnableWhen(EnablePredicate enabled, const void* context = nullptr) {
    SetEnabled(enabled, context);
    return *this;
  }
  IntSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }
  // Slider drag range. When ClampRange has not been set, this is also the
  // accepted input range.
  IntSetting& Range(int lo, int hi) {
    ui_lo_ = lo;
    ui_hi_ = hi;
    if (!has_clamp_range_) {
      lo_ = lo;
      hi_ = hi;
    }
    return *this;
  }
  // Absolute bounds for typed input, programmatic writes, and loads. Takes
  // authority over Range: the slider is narrowed to fit.
  IntSetting& ClampRange(int lo, int hi) {
    lo_ = lo;
    hi_ = hi;
    has_clamp_range_ = true;
    return *this;
  }
  IntSetting& NoSave() { meta_.persist = false; return *this; }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  int default_ = 0;
  // Slider bounds. Unbounded by default so ClampRange alone decides the
  // slider until Range is called.
  int ui_lo_ = std::numeric_limits<int>::lowest();
  int ui_hi_ = std::numeric_limits<int>::max();
  // Accepted input bounds. Range populates these when ClampRange is absent.
  int lo_ = 0;
  int hi_ = 1;
  bool has_clamp_range_ = false;
};

// ---------------------------------------------------------------------------
// BoolSetting
// ---------------------------------------------------------------------------

class BoolSetting : public Setting {
 public:
  BoolSetting(const char* name, const char* label);
  BoolSetting(const char* name, const char* label, bool default_value);

  BoolSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  BoolSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  BoolSetting& Category(const char* text) { meta_.category = CStrToString(text); return *this; }
  BoolSetting& EnableWhen(EnablePredicate enabled, const void* context = nullptr) {
    SetEnabled(enabled, context);
    return *this;
  }
  BoolSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }
  BoolSetting& CategoryToggle(bool on = true) { meta_.category_toggle = on; return *this; }
  BoolSetting& NoSave() { meta_.persist = false; return *this; }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  bool default_ = false;
};

// ---------------------------------------------------------------------------
// StringSetting
// ---------------------------------------------------------------------------

// A free-text setting. The value is a std::string owned by the framework's
// Value, so it copies and compares by content -- which is what an override
// check needs ("did the user type something different from the default?").
//
// `MaxLength` is a length cap, not a validation rule: the row truncates on
// edit, and the clamp on load truncates too, so a hand-edited config cannot
// install a longer value than the UI would have accepted.
class StringSetting : public Setting {
 public:
  StringSetting(const char* name, const char* label);
  StringSetting(const char* name, const char* label, std::string default_value);

  StringSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  StringSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  StringSetting& Category(const char* text) { meta_.category = CStrToString(text); return *this; }
  StringSetting& EnableWhen(EnablePredicate enabled, const void* context = nullptr) {
    SetEnabled(enabled, context);
    return *this;
  }
  StringSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }
  StringSetting& NoSave() { meta_.persist = false; return *this; }
  StringSetting& MaxLength(size_t length) { max_length_ = length; return *this; }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  std::string default_;
  size_t max_length_ = 0;  // 0 = no cap
};

// ---------------------------------------------------------------------------
// IntListSetting
// ---------------------------------------------------------------------------

class IntListSetting : public Setting {
 public:
  IntListSetting(const char* name, const char* label,
                 std::vector<int64_t> default_value = {});

  IntListSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  IntListSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  IntListSetting& MaxItems(size_t count) { max_items_ = count; return *this; }
  IntListSetting& MinItems(size_t count) { min_items_ = count; return *this; }
  IntListSetting& UniqueItems(bool enabled = true) { unique_items_ = enabled; return *this; }
  IntListSetting& RangeEach(int64_t lo, int64_t hi) { lo_ = lo; hi_ = hi; has_range_ = true; return *this; }
  IntListSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  std::vector<int64_t> default_;
  size_t min_items_ = 0;
  size_t max_items_ = 0;
  bool unique_items_ = false;
  bool has_range_ = false;
  int64_t lo_ = 0;
  int64_t hi_ = 0;
};

// ---------------------------------------------------------------------------
// EnumSetting
// ---------------------------------------------------------------------------

class EnumSetting : public Setting {
 public:
  // An item: the text shown, and an optional tooltip.
  struct Item {
    const char* name = nullptr;
    const char* tip = nullptr;
  };

  // Where a setting's items come from. Called at most once, on first use, so
  // a table that lives in another translation unit (or is computed) can be
  // supplied at declaration time just as easily as a literal array.
  using ItemProvider = std::vector<Item> (*)();

  EnumSetting(const char* name, const char* label);
  EnumSetting(const char* name, const char* label, int default_value);

  // Items are displayed in the order given; the value is the item index.
  // Either form may be used, whichever reads better at the declaration site.
  EnumSetting& Items(const char* const* names, const char* const* tips,
                     int count);
  EnumSetting& Items(ItemProvider provider);

  EnumSetting& AsGroup(bool on = true) {
    meta_.enum_presentation = on ? EnumPresentation::GroupButtons
                                 : EnumPresentation::Combo;
    return *this;
  }
  EnumSetting& Tooltip(const char* text) { meta_.tooltip = CStrToString(text); return *this; }
  EnumSetting& InlineToggle(const char* bool_setting) { SetInlineToggle(bool_setting); return *this; }
  EnumSetting& Category(const char* text) { meta_.category = CStrToString(text); return *this; }
  EnumSetting& EnableWhen(EnablePredicate enabled, const void* context = nullptr) {
    SetEnabled(enabled, context);
    return *this;
  }
  EnumSetting& VisibleWhen(VisibilityPredicate visible, const void* context = nullptr) {
    SetVisible(visible, context);
    return *this;
  }
  EnumSetting& NoSave() { meta_.persist = false; return *this; }

  Value DefaultValue() const override;
  void Clamp(Value& value) const override;
  bool Assign(Value& out, const Value& in) const override;
  std::string ToString(const Value& value) const override;
  bool FromString(const std::string& text, Value& out) const override;
  bool Draw(const SettingContext& ctx, const Value& resolved,
            Value& out) const override;

 private:
  // Resolve the provider, if any, exactly once. All item lookups go through
  // this, so an enum declared with a provider behaves identically to one
  // declared with a literal array.
  void EnsureItems() const;

  int default_ = 0;
  ItemProvider provider_ = nullptr;
  mutable bool resolved_ = false;
  mutable std::vector<std::string> names_;
  mutable std::vector<std::string> tips_;
};

}  // namespace ctl
}  // namespace reshade_dlssnr

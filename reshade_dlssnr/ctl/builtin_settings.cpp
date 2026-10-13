#include "ctl/builtin_settings.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "ctl/registry.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {
namespace ctl {

namespace {


}  // namespace

// ---------------------------------------------------------------------------
// FloatSetting
// ---------------------------------------------------------------------------

FloatSetting::FloatSetting(const char* name, const char* label) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}
FloatSetting::FloatSetting(const char* name, const char* label, float default_value)
    : default_(default_value) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

Value FloatSetting::DefaultValue() const { return Value::Float(default_); }

void FloatSetting::Clamp(Value& value) const {
  if (value.kind != Value::Kind::Float) return;
  value.as_float = std::clamp(value.as_float, lo_, hi_);
}

bool FloatSetting::Assign(Value& out, const Value& in) const {
  if (in.kind == Value::Kind::Float) {
    out = in;
  } else if (in.kind == Value::Kind::Int || in.kind == Value::Kind::Enum) {
    out = Value::Float(static_cast<float>(in.as_int));
  } else if (in.kind == Value::Kind::Bool) {
    out = Value::Float(in.as_bool ? 1.0f : 0.0f);
  } else {
    return false;
  }
  Clamp(out);
  return true;
}

std::string FloatSetting::ToString(const Value& value) const {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), format_.c_str(), static_cast<double>(value.as_float));
  return buffer;
}

bool FloatSetting::FromString(const std::string& text, Value& out) const {
  if (text.empty()) return false;
  char* end = nullptr;
  const float parsed = std::strtof(text.c_str(), &end);
  if (end == text.c_str()) return false;
  out = Value::Float(parsed);
  Clamp(out);
  return true;
}

bool FloatSetting::Draw(const SettingContext& ctx, const Value& resolved,
                        Value& out) const {
  float display = resolved.as_float;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(meta_.label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  ImGui::SameLine(kSliderLabelWidth);

  const float trailing = kResetButtonWidth;
  ImGui::SetNextItemWidth(std::max(
      60.0f, ImGui::GetContentRegionAvail().x - trailing -
                 ImGui::GetStyle().ItemSpacing.x));

  bool changed = false;
  {
    const std::string id = std::string("##") + meta_.name;
    // Interactive even while inheriting: editing creates an override on this
    // instance, and the revert affordance appears on the next frame. This
    // matches the natural cascading-form expectation (edit here means
    // "override here"), instead of forcing the user to click R first.
    changed = ImGui::SliderFloat(id.c_str(), &display, lo_, hi_, format_.c_str());
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }

  if (changed) {
    Value candidate = Value::Float(display);
    Clamp(candidate);
    out = candidate;
  } else {
    changed = false;
  }

  ImGui::SameLine();
  ImGui::PushID(meta_.name.c_str());
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    // Reset writes the declared default as an override on this instance.
    out = DefaultValue();
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset to the declared default.");
  }
  ImGui::PopID();


  return changed;
}

// ---------------------------------------------------------------------------
// IntSetting
// ---------------------------------------------------------------------------

IntSetting::IntSetting(const char* name, const char* label) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}
IntSetting::IntSetting(const char* name, const char* label, int default_value)
    : default_(default_value) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

Value IntSetting::DefaultValue() const { return Value::Int(default_); }

void IntSetting::Clamp(Value& value) const {
  if (value.kind != Value::Kind::Int && value.kind != Value::Kind::Enum) return;
  value.as_int = std::clamp(value.as_int, lo_, hi_);
}

bool IntSetting::Assign(Value& out, const Value& in) const {
  if (in.kind == Value::Kind::Int || in.kind == Value::Kind::Enum) {
    out = Value::Int(in.as_int);
  } else if (in.kind == Value::Kind::Float) {
    out = Value::Int(static_cast<int>(in.as_float));
  } else if (in.kind == Value::Kind::Bool) {
    out = Value::Int(in.as_bool ? 1 : 0);
  } else {
    return false;
  }
  Clamp(out);
  return true;
}

std::string IntSetting::ToString(const Value& value) const {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d", value.as_int);
  return buffer;
}

bool IntSetting::FromString(const std::string& text, Value& out) const {
  if (text.empty()) return false;
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str()) return false;
  out = Value::Int(static_cast<int>(parsed));
  Clamp(out);
  return true;
}

bool IntSetting::Draw(const SettingContext& ctx, const Value& resolved,
                      Value& out) const {
  int display = resolved.as_int;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(meta_.label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  ImGui::SameLine(kSliderLabelWidth);

  const float trailing = kResetButtonWidth;
  ImGui::SetNextItemWidth(std::max(
      60.0f, ImGui::GetContentRegionAvail().x - trailing -
                 ImGui::GetStyle().ItemSpacing.x));

  bool changed = false;
  {
    const std::string id = std::string("##") + meta_.name;
    // Interactive even while inheriting: editing creates an override.
    changed = ImGui::SliderInt(id.c_str(), &display, lo_, hi_);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  if (changed) {
    Value candidate = Value::Int(display);
    Clamp(candidate);
    out = candidate;
  } else {
    changed = false;
  }

  ImGui::SameLine();
  ImGui::PushID(meta_.name.c_str());
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    out = DefaultValue();
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset to the declared default.");
  }
  ImGui::PopID();

  return changed;
}

// ---------------------------------------------------------------------------
// BoolSetting
// ---------------------------------------------------------------------------

BoolSetting::BoolSetting(const char* name, const char* label) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}
BoolSetting::BoolSetting(const char* name, const char* label, bool default_value)
    : default_(default_value) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

Value BoolSetting::DefaultValue() const { return Value::Bool(default_); }

void BoolSetting::Clamp(Value&) const {}

bool BoolSetting::Assign(Value& out, const Value& in) const {
  if (in.kind == Value::Kind::Bool) {
    out = in;
  } else if (in.kind == Value::Kind::Int || in.kind == Value::Kind::Enum) {
    out = Value::Bool(in.as_int != 0);
  } else if (in.kind == Value::Kind::Float) {
    out = Value::Bool(in.as_float != 0.0f);
  } else {
    return false;
  }
  return true;
}

std::string BoolSetting::ToString(const Value& value) const {
  return value.as_bool ? "1" : "0";
}

bool BoolSetting::FromString(const std::string& text, Value& out) const {
  if (text == "1" || text == "true") {
    out = Value::Bool(true);
    return true;
  }
  if (text == "0" || text == "false") {
    out = Value::Bool(false);
    return true;
  }
  return false;
}

bool BoolSetting::Draw(const SettingContext& ctx, const Value& resolved,
                       Value& out) const {
  bool display = resolved.as_bool;

  ImGui::AlignTextToFramePadding();
  bool changed = false;
  {
    const std::string id = std::string("##") + meta_.name;
    // Interactive even while inheriting: toggling creates an override.
    changed = ImGui::Checkbox(id.c_str(), &display);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(meta_.label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }

  if (changed) {
    out = Value::Bool(display);
  } else {
    changed = false;
  }

  return changed;
}

// ---------------------------------------------------------------------------
// StringSetting
// ---------------------------------------------------------------------------

StringSetting::StringSetting(const char* name, const char* label) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

StringSetting::StringSetting(const char* name, const char* label,
                             std::string default_value)
    : default_(std::move(default_value)) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

Value StringSetting::DefaultValue() const { return Value::String(default_); }

void StringSetting::Clamp(Value& value) const {
  if (value.kind != Value::Kind::String) return;
  if (max_length_ > 0 && value.as_string.size() > max_length_) {
    value.as_string.resize(max_length_);
  }
}

bool StringSetting::Assign(Value& out, const Value& in) const {
  if (in.kind != Value::Kind::String) return false;
  out = in;
  return true;
}

std::string StringSetting::ToString(const Value& value) const {
  if (value.kind != Value::Kind::String) return default_;
  return value.as_string;
}

bool StringSetting::FromString(const std::string& text, Value& out) const {
  Value candidate = Value::String(text);
  Clamp(candidate);
  out = candidate;
  return true;
}

bool StringSetting::Draw(const SettingContext& ctx, const Value& resolved,
                         Value& out) const {
  // InputText needs a mutable NUL-terminated buffer, so the value is copied
  // into a fixed array and copied back only on an actual edit.
  std::string text =
      resolved.kind == Value::Kind::String ? resolved.as_string : default_;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(meta_.label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  ImGui::SameLine(kControlLabelWidth);

  const float trailing = kResetButtonWidth;
  ImGui::SetNextItemWidth(std::max(
      60.0f, ImGui::GetContentRegionAvail().x - trailing -
                 ImGui::GetStyle().ItemSpacing.x));

  bool edited = false;
  {
    const std::string id = std::string("##") + meta_.name;
    // Bounded by the cap the setting declared, so a longer string simply
    // cannot be typed. With no cap, fall back to a generous fixed buffer:
    // InputText has no std::string overload and needs somewhere to write.
    const size_t capacity = max_length_ > 0 ? max_length_ : 255;
    std::vector<char> buffer(capacity + 1, '\0');
    std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
    // Interactive even while inheriting: typing creates an override.
    edited = ImGui::InputText(id.c_str(), buffer.data(), buffer.size());
    if (edited) text.assign(buffer.data());
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }

  if (edited && text !=
          (resolved.kind == Value::Kind::String ? resolved.as_string
                                                : default_)) {
    Value candidate = Value::String(text);
    Clamp(candidate);
    out = candidate;
  } else {
    edited = false;
  }

  ImGui::SameLine();
  ImGui::PushID(meta_.name.c_str());
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    // Reset writes the declared default as an override on this instance.
    out = DefaultValue();
    edited = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset to the declared default.");
  }
  ImGui::PopID();

  return edited;
}

// ---------------------------------------------------------------------------
// EnumSetting
// ---------------------------------------------------------------------------

EnumSetting::EnumSetting(const char* name, const char* label) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}
EnumSetting::EnumSetting(const char* name, const char* label, int default_value)
    : default_(default_value) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

EnumSetting& EnumSetting::Items(const char* const* names,
                                const char* const* tips, int count) {
  names_.clear();
  tips_.clear();
  if (names == nullptr || count <= 0) {
    resolved_ = true;
    return *this;
  }

  names_.reserve(static_cast<size_t>(count));
  tips_.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    std::string name = CStrToString(names[i]);
    std::string tip = CStrToString(tips != nullptr ? tips[i] : nullptr);
    if (tips == nullptr || tips[i] == nullptr) tip = name;
    names_.push_back(std::move(name));
    tips_.push_back(std::move(tip));
  }
  resolved_ = true;
  return *this;
}

EnumSetting& EnumSetting::Items(ItemProvider provider) {
  provider_ = provider;
  resolved_ = false;
  return *this;
}

void EnumSetting::EnsureItems() const {
  if (resolved_) return;
  resolved_ = true;
  if (provider_ == nullptr) return;
  for (const Item& item : provider_()) {
    std::string name = CStrToString(item.name);
    std::string tip = CStrToString(item.tip);
    if (item.tip == nullptr) tip = name;
    names_.push_back(std::move(name));
    tips_.push_back(std::move(tip));
  }
}

Value EnumSetting::DefaultValue() const { return Value::Enum(default_); }

void EnumSetting::Clamp(Value& value) const {
  if (value.kind != Value::Kind::Enum && value.kind != Value::Kind::Int) return;
  EnsureItems();
  const int count = static_cast<int>(names_.size());
  if (count <= 0) return;
  if (value.as_int < 0) value.as_int = 0;
  if (value.as_int >= count) value.as_int = count - 1;
}

bool EnumSetting::Assign(Value& out, const Value& in) const {
  if (in.kind == Value::Kind::Enum || in.kind == Value::Kind::Int) {
    out = Value::Enum(in.as_int);
  } else {
    return false;
  }
  Clamp(out);
  return true;
}

std::string EnumSetting::ToString(const Value& value) const {
  EnsureItems();
  Clamp(const_cast<Value&>(value));
  const int idx = value.as_int;
  if (idx >= 0 && idx < static_cast<int>(names_.size())) return names_[idx];
  return std::to_string(idx);
}

bool EnumSetting::FromString(const std::string& text, Value& out) const {
  EnsureItems();
  for (size_t i = 0; i < names_.size(); ++i) {
    if (text == names_[i]) {
      out = Value::Enum(static_cast<int>(i));
      return true;
    }
  }
  // Fall back to a numeric index so an older file keeps loading.
  char* end = nullptr;
  const long parsed = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str()) return false;
  out = Value::Enum(static_cast<int>(parsed));
  Clamp(out);
  return true;
}

bool EnumSetting::Draw(const SettingContext& ctx, const Value& resolved,
                       Value& out) const {
  EnsureItems();
  const int count = static_cast<int>(names_.size());
  int display = resolved.as_int;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(meta_.label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }
  ImGui::SameLine(kControlLabelWidth);

  bool changed = false;
  ImGui::SetNextItemWidth(-1.0f);

  // Interactive even while inheriting: selecting creates an override.
  {
    if (meta_.enum_presentation == EnumPresentation::GroupButtons) {
      std::vector<const char*> labels;
      std::vector<const char*> tooltips;
      labels.reserve(names_.size());
      tooltips.reserve(tips_.size());
      for (const std::string& name : names_) labels.push_back(name.c_str());
      for (const std::string& tip : tips_) tooltips.push_back(tip.c_str());
      const int clicked = DrawGroupButtons(meta_.name.c_str(), labels.data(),
                                           tooltips.data(), count, display,
                                           nullptr);
      if (clicked >= 0 && clicked != display) {
        display = clicked;
        changed = true;
      }
    } else {
      const std::string id = std::string("##") + meta_.name;
      const std::string preview_id = std::string("##") + meta_.name + "_preview";
      const std::string preview =
          (display >= 0 && display < count) ? names_[display] : std::string();
      if (ImGui::BeginCombo(preview_id.c_str(), preview.c_str())) {
        for (int i = 0; i < count; ++i) {
          const bool selected = (i == display);
          if (ImGui::Selectable(names_[i].c_str(), selected) && i != display) {
            display = i;
            changed = true;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip("%s", tips_[i].c_str());
          }
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      (void)id;
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", meta_.tooltip.empty() ? meta_.label.c_str() : meta_.tooltip.c_str());
  }

  if (changed) {
    Value candidate = Value::Enum(display);
    Clamp(candidate);
    out = candidate;
  } else {
    changed = false;
  }

  return changed;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void RegisterBuiltinTypes(Registry& registry) {
  registry.Register("float", [] { return std::make_unique<FloatSetting>("", ""); });
  registry.Register("int", [] { return std::make_unique<IntSetting>("", ""); });
  registry.Register("bool", [] { return std::make_unique<BoolSetting>("", ""); });
  registry.Register("enum", [] { return std::make_unique<EnumSetting>("", ""); });
  registry.Register("string", [] { return std::make_unique<StringSetting>("", ""); });
}

}  // namespace ctl
}  // namespace reshade_dlssnr

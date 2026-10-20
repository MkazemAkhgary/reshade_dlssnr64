#include "ctl/builtin_settings.h"
#include "profiling/profile.h"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cctype>
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
  DLSSNR_PROFILE_CTL_L3("FloatSetting::Clamp");
  if (value.kind != Value::Kind::Float) return;
  value.as_float = std::clamp(value.as_float, lo_, hi_);
}

bool FloatSetting::Assign(Value& out, const Value& in) const {
  DLSSNR_PROFILE_CTL_L3("FloatSetting::Assign");
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
  DLSSNR_PROFILE_CTL_L3("FloatSetting::ToString");
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), format_.c_str(), static_cast<double>(value.as_float));
  return buffer;
}

bool FloatSetting::FromString(const std::string& text, Value& out) const {
  DLSSNR_PROFILE_CTL_L3("FloatSetting::FromString");
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
  DLSSNR_PROFILE_UI_L3("FloatSetting::Draw");
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
    // Range, narrowed by ClampRange when the latter is set. Typed input is
    // always clamped to [lo_, hi_].
    const float eff_lo = has_clamp_range_ ? std::max(ui_lo_, lo_) : lo_;
    const float eff_hi = has_clamp_range_ ? std::min(ui_hi_, hi_) : hi_;
    changed = ImGui::SliderFloat(id.c_str(), &display, eff_lo, eff_hi,
                                 format_.c_str());
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
  DLSSNR_PROFILE_CTL_L3("IntSetting::Clamp");
  if (value.kind != Value::Kind::Int && value.kind != Value::Kind::Enum) return;
  value.as_int = std::clamp(value.as_int, lo_, hi_);
}

bool IntSetting::Assign(Value& out, const Value& in) const {
  DLSSNR_PROFILE_CTL_L3("IntSetting::Assign");
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
  DLSSNR_PROFILE_CTL_L3("IntSetting::ToString");
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d", value.as_int);
  return buffer;
}

bool IntSetting::FromString(const std::string& text, Value& out) const {
  DLSSNR_PROFILE_CTL_L3("IntSetting::FromString");
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
  DLSSNR_PROFILE_UI_L3("IntSetting::Draw");
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
    // Range, narrowed by ClampRange when the latter is set. Typed input is
    // always clamped to [lo_, hi_].
    const int eff_lo = has_clamp_range_ ? std::max(ui_lo_, lo_) : lo_;
    const int eff_hi = has_clamp_range_ ? std::min(ui_hi_, hi_) : hi_;
    changed = ImGui::SliderInt(id.c_str(), &display, eff_lo, eff_hi);
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
  DLSSNR_PROFILE_CTL_L3("BoolSetting::Assign");
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
  DLSSNR_PROFILE_CTL_L3("BoolSetting::ToString");
  return value.as_bool ? "1" : "0";
}

bool BoolSetting::FromString(const std::string& text, Value& out) const {
  DLSSNR_PROFILE_CTL_L3("BoolSetting::FromString");
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
  DLSSNR_PROFILE_UI_L3("BoolSetting::Draw");
  bool display = resolved.as_bool;

  ImGui::AlignTextToFramePadding();
  bool changed = false;
  {
    const std::string id = std::string("##") + meta_.name;
    // Toggling writes the value on this instance.
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
  DLSSNR_PROFILE_CTL_L3("StringSetting::Clamp");
  if (value.kind != Value::Kind::String) return;
  if (max_length_ > 0 && value.as_string.size() > max_length_) {
    value.as_string.resize(max_length_);
  }
}

bool StringSetting::Assign(Value& out, const Value& in) const {
  DLSSNR_PROFILE_CTL_L3("StringSetting::Assign");
  if (in.kind != Value::Kind::String) return false;
  out = in;
  return true;
}

std::string StringSetting::ToString(const Value& value) const {
  DLSSNR_PROFILE_CTL_L3("StringSetting::ToString");
  if (value.kind != Value::Kind::String) return default_;
  return value.as_string;
}

bool StringSetting::FromString(const std::string& text, Value& out) const {
  DLSSNR_PROFILE_CTL_L3("StringSetting::FromString");
  Value candidate = Value::String(text);
  Clamp(candidate);
  out = candidate;
  return true;
}

bool StringSetting::Draw(const SettingContext& ctx, const Value& resolved,
                         Value& out) const {
  DLSSNR_PROFILE_UI_L3("StringSetting::Draw");
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
    // Typing writes the value on this instance.
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
// IntListSetting
// ---------------------------------------------------------------------------

IntListSetting::IntListSetting(const char* name, const char* label,
                               std::vector<int64_t> default_value)
    : default_(std::move(default_value)) {
  meta_.name = CStrToString(name);
  meta_.label = CStrToString(label);
}

Value IntListSetting::DefaultValue() const { return Value::IntList(default_); }

void IntListSetting::Clamp(Value& value) const {
  DLSSNR_PROFILE_CTL_L3("IntListSetting::Clamp");
  if (value.kind != Value::Kind::IntList) return;
  std::vector<int64_t> result;
  result.reserve(std::min(value.as_int_list.size(), max_items_));
  for (int64_t item : value.as_int_list) {
    if (has_range_) item = std::clamp(item, lo_, hi_);
    if (unique_items_ &&
        std::find(result.begin(), result.end(), item) != result.end()) {
      continue;
    }
    result.push_back(item);
    if (max_items_ > 0 && result.size() >= max_items_) break;
  }
  if (result.size() < min_items_) result = default_;
  if (max_items_ > 0 && result.size() > max_items_) {
    result.resize(max_items_);
  }
  value.as_int_list = std::move(result);
}

bool IntListSetting::Assign(Value& out, const Value& in) const {
  DLSSNR_PROFILE_CTL_L3("IntListSetting::Assign");
  if (in.kind != Value::Kind::IntList) return false;
  out = in;
  Clamp(out);
  return true;
}

std::string IntListSetting::ToString(const Value& value) const {
  DLSSNR_PROFILE_CTL_L3("IntListSetting::ToString");
  if (value.kind != Value::Kind::IntList) return {};
  std::string text;
  for (int64_t item : value.as_int_list) {
    if (!text.empty()) text.push_back(',');
    text += std::to_string(item);
  }
  return text;
}

bool IntListSetting::FromString(const std::string& text, Value& out) const {
  DLSSNR_PROFILE_CTL_L3("IntListSetting::FromString");
  std::vector<int64_t> values;
  size_t begin = 0;
  while (begin < text.size()) {
    const size_t comma = text.find(',', begin);
    const size_t end = comma == std::string::npos ? text.size() : comma;
    size_t token_begin = begin;
    size_t token_end = end;
    while (token_begin < token_end &&
           std::isspace(static_cast<unsigned char>(text[token_begin]))) {
      ++token_begin;
    }
    while (token_end > token_begin &&
           std::isspace(static_cast<unsigned char>(text[token_end - 1]))) {
      --token_end;
    }
    if (token_begin == token_end) return false;

    int64_t value = 0;
    const char* first = text.data() + token_begin;
    const char* last = text.data() + token_end;
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc() || parsed.ptr != last) return false;
    values.push_back(value);
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  if (!text.empty() && text.back() == ',') return false;
  Value candidate = Value::IntList(std::move(values));
  Clamp(candidate);
  out = std::move(candidate);
  return true;
}

bool IntListSetting::Draw(const SettingContext&, const Value& resolved,
                          Value& out) const {
  DLSSNR_PROFILE_UI_L3("IntListSetting::Draw");
  const std::string current = ToString(resolved);
  std::vector<char> buffer(std::max<size_t>(current.size() + 64, 128), '\0');
  std::snprintf(buffer.data(), buffer.size(), "%s", current.c_str());
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
  const std::string id = std::string("##") + meta_.name;
  bool changed = ImGui::InputText(id.c_str(), buffer.data(), buffer.size());
  if (changed) {
    Value candidate;
    if (FromString(buffer.data(), candidate) && candidate != resolved) {
      out = std::move(candidate);
    } else {
      changed = false;
    }
  }
  ImGui::SameLine();
  ImGui::PushID(meta_.name.c_str());
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    out = DefaultValue();
    changed = true;
  }
  ImGui::PopID();
  return changed;
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

  // Selecting writes the value on this instance.
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
  registry.Register("int_list", [] { return std::make_unique<IntListSetting>("", ""); });
}

}  // namespace ctl
}  // namespace reshade_dlssnr

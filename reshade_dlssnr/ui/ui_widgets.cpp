#include <imgui.h>
#include <reshade.hpp>

#include "ui/ui_widgets.h"

#include "feature/ui_state/view.h"
#include "ctl/types.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace reshade_dlssnr {

bool g_show_tooltips = true;

void Tip(const char* fmt, ...) {
  DLSSNR_PROFILE_UI_L3("UI Tooltip");
  if (!g_show_tooltips || fmt == nullptr) return;
  va_list args;
  va_start(args, fmt);
  ImGui::SetTooltipV(fmt, args);
  va_end(args);
}

void SectionDivider() {
  DLSSNR_PROFILE_UI_L3("UI Section Divider");
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::Spacing();
}

namespace {

void DrawHeaderToggle(const char* label, bool& changed,
                      const HeaderControls& controls) {
  DLSSNR_PROFILE_UI_L3("UI Header Toggle");
  if (controls.enabled == nullptr && controls.enabled_float == nullptr) return;
  if (controls.enabled_disabled) ImGui::BeginDisabled();

  bool float_value = controls.enabled_float != nullptr &&
                     *controls.enabled_float >= 0.5f;
  bool* checkbox_value = controls.enabled != nullptr
                             ? controls.enabled
                             : &float_value;
  if (ImGui::Checkbox("##header_enabled", checkbox_value)) {
    if (controls.enabled_float != nullptr) {
      *controls.enabled_float = float_value ? 1.0f : 0.0f;
    }
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const std::string enabled_tooltip =
        CStrToString(controls.enabled_tooltip);
    if (!enabled_tooltip.empty()) {
      Tip("%s", enabled_tooltip.c_str());
    } else {
      Tip("Enable or disable %s.", label);
    }
  }
  if (controls.enabled_disabled) ImGui::EndDisabled();
  ImGui::SameLine();
}

void DrawHeaderReset(const char* label, bool& changed,
                     const HeaderControls& controls) {
  DLSSNR_PROFILE_UI_L3("UI Header Reset");
  if (controls.reset == nullptr) return;
  const float right = ImGui::GetCursorPosX() +
                       ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(std::max(0.0f, right - kResetButtonWidth));
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    controls.reset(controls.reset_context);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset all %s settings to their defaults.", label);
  }
}

}  // namespace

bool PersistentSettingHeader(const char* label, const char* setting_key,
                             bool& changed, const char* tooltip,
                             HeaderControls controls) {
  DLSSNR_PROFILE_UI_L3("UI Persistent Setting Header");
  bool open = feature::ui_state::Read(setting_key);
  const bool previous = open;
  const bool expanded = PersistentHeader(label, open, changed, tooltip, controls);
  if (open != previous) feature::ui_state::Write(setting_key, open);
  return expanded;
}

bool PersistentHeader(const char* label, bool& open_setting, bool& changed,
                      const char* tooltip, HeaderControls controls) {
  DLSSNR_PROFILE_UI_L3("UI Persistent Header");
  const std::string label_storage = CStrToString(label);
  const std::string tooltip_storage = CStrToString(tooltip);
  label = label_storage.c_str();
  ImGui::PushID(label);
  DrawHeaderToggle(label, changed, controls);
  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader |
                                      ImGuiTreeNodeFlags_AllowOverlap);
  const bool header_hovered =
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  if (header_hovered) {
    if (!tooltip_storage.empty()) {
      Tip("%s", tooltip_storage.c_str());
    } else {
      Tip("Expand or collapse the %s section.", label);
    }
  }
  DrawHeaderReset(label, changed, controls);
  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  ImGui::PopID();
  return open;
}

bool PersistentSubHeader(const char* label, bool& open_setting,
                         bool& changed, const char* tooltip,
                         HeaderControls controls) {
  DLSSNR_PROFILE_UI_L3("UI Persistent Subheader");
  const std::string label_storage = CStrToString(label);
  const std::string tooltip_storage = CStrToString(tooltip);
  label = label_storage.c_str();
  const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  const ImVec4 tint(base.x * 0.82f, base.y * 0.82f, base.z * 0.92f, base.w);

  const ImVec4 hdr_hover = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
  const ImVec4 hdr_active = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);

  ImGui::PushID(label);
  DrawHeaderToggle(label, changed, controls);
  ImGui::PushStyleColor(ImGuiCol_Text, tint);
  ImGui::PushStyleColor(ImGuiCol_Header,
                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(
      ImGuiCol_HeaderHovered,
      ImVec4(hdr_hover.x, hdr_hover.y, hdr_hover.z, hdr_hover.w * 0.30f));
  ImGui::PushStyleColor(
      ImGuiCol_HeaderActive,
      ImVec4(hdr_active.x, hdr_active.y, hdr_active.z, hdr_active.w * 0.50f));

  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader |
                                      ImGuiTreeNodeFlags_AllowOverlap);
  const bool header_hovered =
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  ImGui::PopStyleColor(4);
  if (header_hovered) {
    if (!tooltip_storage.empty()) {
      Tip("%s", tooltip_storage.c_str());
    } else {
      Tip("Expand or collapse the %s subsection.", label);
    }
  }

  DrawHeaderReset(label, changed, controls);
  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  ImGui::PopID();
  return open;
}

bool DrawFloatRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  float lo,
                  float hi,
                  float default_value) {
  DLSSNR_PROFILE_UI_L3("UI Float Row");
  const std::string id_storage = CStrToString(id);
  id = id_storage.c_str();
  const std::string label_storage = CStrToString(label);
  label = label_storage.c_str();
  bool changed = false;
  std::string tooltip_storage = CStrToString(tooltip);
  char fallback_tooltip[256];
  if (tooltip_storage.empty()) {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Adjust %s between %.4g and %.4g. Reset returns to %.4g.",
                  label, static_cast<double>(lo), static_cast<double>(hi),
                  static_cast<double>(default_value));
    tooltip_storage = fallback_tooltip;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine(kSliderLabelWidth);

  const float slider_width = std::max(
      60.0f, ImGui::GetContentRegionAvail().x - kResetButtonWidth -
                 ImGui::GetStyle().ItemSpacing.x);
  ImGui::SetNextItemWidth(slider_width);

  const std::string slider_id = std::string("##") + id;
  if (ImGui::SliderFloat(slider_id.c_str(), raw_value, lo, hi, "%.3f")) {
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *raw_value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset to default");
  }
  ImGui::PopID();

  return changed;
}

bool DrawIntRow(const char* id,
                const char* label,
                const char* tooltip,
                int* raw_value,
                int lo,
                int hi,
                int default_value) {
  DLSSNR_PROFILE_UI_L3("UI Integer Row");
  const std::string id_storage = CStrToString(id);
  id = id_storage.c_str();
  const std::string label_storage = CStrToString(label);
  label = label_storage.c_str();
  bool changed = false;
  std::string tooltip_storage = CStrToString(tooltip);
  char fallback_tooltip[256];
  if (tooltip_storage.empty()) {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Adjust %s between %d and %d. Reset returns to %d.",
                  label, lo, hi, default_value);
    tooltip_storage = fallback_tooltip;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine(kSliderLabelWidth);
  const float slider_width = std::max(
      60.0f, ImGui::GetContentRegionAvail().x - kResetButtonWidth -
                 ImGui::GetStyle().ItemSpacing.x);
  ImGui::SetNextItemWidth(slider_width);

  const std::string slider_id = std::string("##") + id;
  if (ImGui::SliderInt(slider_id.c_str(), raw_value, lo, hi)) {
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *raw_value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset to default");
  }
  ImGui::PopID();

  return changed;
}

bool DrawCheckboxAtLabel(const char* id, bool* value, const char* tooltip) {
  DLSSNR_PROFILE_UI_L3("UI Checkbox Row");
  const std::string id_storage = CStrToString(id);
  const std::string tooltip_storage = CStrToString(tooltip);
  id = id_storage.c_str();
  const std::string cb_id = std::string("##") + id;
  const bool changed = ImGui::Checkbox(cb_id.c_str(), value);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (!tooltip_storage.empty()) {
      Tip("%s", tooltip_storage.c_str());
    } else {
      Tip("Toggle this option.");
    }
  }
  return changed;
}

bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  const char* const* item_tooltips,
                  int item_count) {
  DLSSNR_PROFILE_UI_L3("UI Combo Row");
  const std::string id_storage = CStrToString(id);
  const std::string label_storage = CStrToString(label);
  std::string tooltip_storage = CStrToString(tooltip);
  id = id_storage.c_str();
  label = label_storage.c_str();
  if (items == nullptr || item_count <= 0) return false;
  char fallback_tooltip[192];
  if (tooltip_storage.empty()) {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Choose the %s option.", label);
    tooltip_storage = fallback_tooltip;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);

  const int clamped = (*value >= 0 && *value < item_count) ? *value : 0;
  const std::string preview = CStrToString(items[clamped]);
  const std::string combo_id = std::string("##") + id;

  bool changed = false;
  if (ImGui::BeginCombo(combo_id.c_str(), preview.c_str())) {
    for (int i = 0; i < item_count; ++i) {
      const bool selected = (i == clamped);
      const std::string item_label = CStrToString(items[i]);
      const bool has_item_tooltip =
          item_tooltips != nullptr && item_tooltips[i] != nullptr;
      const std::string item_tooltip =
          CStrToString(has_item_tooltip ? item_tooltips[i] : nullptr);
      if (ImGui::Selectable(item_label.c_str(), selected)) {
        if (*value != i) {
          *value = i;
          changed = true;
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (has_item_tooltip) {
          Tip("%s", item_tooltip.c_str());
        } else {
          Tip("Select %s.", item_label.c_str());
        }
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const bool has_preview_tooltip =
        item_tooltips != nullptr && item_tooltips[clamped] != nullptr;
    const std::string preview_tooltip =
        CStrToString(has_preview_tooltip ? item_tooltips[clamped] : nullptr);
    if (has_preview_tooltip) {
      Tip("%s", preview_tooltip.c_str());
    } else {
      Tip("Current %s: %s.", label, preview.c_str());
    }
  }

  return changed;
}

int DrawGroupButtons(const char* id,
                     const char* const* labels,
                     const char* const* tooltips,
                     int count,
                     int selected,
                     const bool* disabled,
                     const bool* unavailable,
                     const char* const* unavailable_reasons,
                     float fixed_button_width) {
  DLSSNR_PROFILE_UI_L3("UI Group Buttons");
  if (count <= 0 || labels == nullptr) return -1;

  const std::string id_storage = CStrToString(id);
  id = id_storage.c_str();
  ImGui::PushID(id);

  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float total_spacing = spacing * static_cast<float>(count - 1);
  const float distributed_width = (avail - total_spacing) /
                                  static_cast<float>(count);
  const float button_width = fixed_button_width > 0.0f
                                 ? fixed_button_width
                                 : std::max(40.0f, distributed_width);

  int clicked = -1;

  for (int i = 0; i < count; ++i) {
    if (i > 0) ImGui::SameLine(0.0f, spacing);

    const bool is_selected = (i == selected);
    const bool is_disabled = (disabled != nullptr && disabled[i]);
    const bool is_unavailable = (unavailable != nullptr && unavailable[i]);

    if (is_disabled) ImGui::BeginDisabled();
    if (is_unavailable) {
      // Still clickable, but visibly not usable right now.
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                          ImGui::GetStyle().Alpha * 0.55f);
    }

    const int pushed = is_selected ? 3 : 0;
    if (is_selected) {
      const ImVec4 accent =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImGui::PushStyleColor(ImGuiCol_Button, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
    }

    const std::string item_label = CStrToString(labels[i]);
    if (ImGui::Button(item_label.c_str(), ImVec2(button_width, 0.0f)) &&
        !is_selected && !is_disabled) {
      clicked = i;
    }

    if (pushed > 0) ImGui::PopStyleColor(pushed);
    if (is_unavailable) {
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      const bool has_reason = is_unavailable && unavailable_reasons != nullptr &&
                              unavailable_reasons[i] != nullptr;
      const bool has_item_tooltip = tooltips != nullptr && tooltips[i] != nullptr;
      const std::string item_tooltip =
          CStrToString(has_item_tooltip ? tooltips[i] : nullptr);
      if (has_reason) {
        Tip("%s", unavailable_reasons[i]);
      } else if (has_item_tooltip) {
        Tip("%s", item_tooltip.c_str());
      } else {
        Tip("Select %s.", item_label.c_str());
      }
    }

    if (is_disabled) ImGui::EndDisabled();
  }

  ImGui::PopID();
  return clicked;
}

bool DrawGroupRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  const char* const* labels,
                  const char* const* item_tooltips,
                  int count,
                  int* value,
                  const bool* disabled) {
  DLSSNR_PROFILE_UI_L3("UI Group Row");
  const std::string id_storage = CStrToString(id);
  id = id_storage.c_str();
  const std::string label_storage = CStrToString(label);
  std::string tooltip_storage = CStrToString(tooltip);
  label = label_storage.c_str();
  char fallback_tooltip[192];
  if (tooltip_storage.empty()) {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Choose the %s setting.", label);
    tooltip_storage = fallback_tooltip;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip_storage.c_str());
  }

  ImGui::SameLine(kControlLabelWidth);

  const int current = (*value >= 0 && *value < count) ? *value : -1;
  const int clicked = DrawGroupButtons(id, labels, item_tooltips, count,
                                       current, disabled);
  if (clicked >= 0) {
    *value = clicked;
    return true;
  }
  return false;
}

}  // namespace reshade_dlssnr

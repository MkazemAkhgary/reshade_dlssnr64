#include <imgui.h>
#include <reshade.hpp>

#include "ui/ui_widgets.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace reshade_dlssnr {

bool g_show_tooltips = true;

void Tip(const char* fmt, ...) {
  if (!g_show_tooltips) return;
  va_list args;
  va_start(args, fmt);
  ImGui::SetTooltipV(fmt, args);
  va_end(args);
}

void SectionDivider() {
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::Spacing();
}

bool PersistentHeader(const char* label, bool& open_setting, bool& changed,
                      const char* tooltip) {
  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (tooltip != nullptr && tooltip[0] != '\0') {
      Tip("%s", tooltip);
    } else {
      Tip("Expand or collapse the %s section.", label);
    }
  }
  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  return open;
}

bool PersistentSubHeader(const char* label, bool& open_setting,
                         bool& changed, const char* tooltip) {
  const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  const ImVec4 tint(base.x * 0.82f, base.y * 0.82f, base.z * 0.92f, base.w);

  const ImVec4 hdr_hover = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
  const ImVec4 hdr_active = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);

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
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader);

  ImGui::PopStyleColor(4);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (tooltip != nullptr && tooltip[0] != '\0') {
      Tip("%s", tooltip);
    } else {
      Tip("Expand or collapse the %s subsection.", label);
    }
  }

  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  return open;
}

bool DrawFloatRow(uint32_t pass_index,
                  const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  bool* inherit_flag,
                  float inherited_value,
                  float lo,
                  float hi,
                  float default_value) {
  bool changed = false;
  char fallback_tooltip[256];
  if (tooltip == nullptr || tooltip[0] == '\0') {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Adjust %s between %.4g and %.4g. Reset returns to %.4g.",
                  label, static_cast<double>(lo), static_cast<double>(hi),
                  static_cast<double>(default_value));
    tooltip = fallback_tooltip;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = (pass_index > 0) && (inherit_flag != nullptr);
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  const float slider_width = std::max(
      60.0f, ImGui::GetContentRegionAvail().x - trailing_width -
                 ImGui::GetStyle().ItemSpacing.x);
  ImGui::SetNextItemWidth(slider_width);

  const bool inheriting = has_inherit && *inherit_flag;
  float display_value = inheriting ? inherited_value : *raw_value;

  const std::string slider_id = std::string("##") + id;
  bool slider_changed = false;
  WithDisabled(inheriting, [&] {
    slider_changed =
        ImGui::SliderFloat(slider_id.c_str(), &display_value, lo, hi, "%.3f");
  });
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
  }
  if (slider_changed && !inheriting) {
    *raw_value = display_value;
    changed = true;
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

  if (has_inherit) {
    ImGui::SameLine();
    const std::string inherit_id = std::string(id) + "_inherit";
    ImGui::PushID(inherit_id.c_str());
    const bool on = *inherit_flag;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      *inherit_flag = !*inherit_flag;
      changed = true;
    }
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Inherit this value from the previous pass");
    }
    ImGui::PopID();
  }

  return changed;
}

bool DrawIntRow(const char* id,
                const char* label,
                const char* tooltip,
                int* raw_value,
                int lo,
                int hi,
                int default_value) {
  bool changed = false;
  char fallback_tooltip[256];
  if (tooltip == nullptr || tooltip[0] == '\0') {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Adjust %s between %d and %d. Reset returns to %d.",
                  label, lo, hi, default_value);
    tooltip = fallback_tooltip;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  const float slider_width = std::max(
      60.0f, ImGui::GetContentRegionAvail().x - kResetButtonWidth -
                 ImGui::GetStyle().ItemSpacing.x);
  ImGui::SetNextItemWidth(slider_width);

  const std::string slider_id = std::string("##") + id;
  if (ImGui::SliderInt(slider_id.c_str(), raw_value, lo, hi)) {
    changed = true;
  }
  if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
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
  const std::string cb_id = std::string("##") + id;
  const bool changed = ImGui::Checkbox(cb_id.c_str(), value);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (tooltip != nullptr && tooltip[0] != '\0') {
      Tip("%s", tooltip);
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
  char fallback_tooltip[192];
  if (tooltip == nullptr || tooltip[0] == '\0') {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Choose the %s option.", label);
    tooltip = fallback_tooltip;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);

  const int clamped = (*value >= 0 && *value < item_count) ? *value : 0;
  const char* preview = items[clamped];
  const std::string combo_id = std::string("##") + id;

  bool changed = false;
  if (ImGui::BeginCombo(combo_id.c_str(), preview)) {
    for (int i = 0; i < item_count; ++i) {
      const bool selected = (i == clamped);
      if (ImGui::Selectable(items[i], selected)) {
        if (*value != i) {
          *value = i;
          changed = true;
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (item_tooltips != nullptr && item_tooltips[i] != nullptr) {
          Tip("%s", item_tooltips[i]);
        } else {
          Tip("Select %s.", items[i]);
        }
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (item_tooltips != nullptr && item_tooltips[clamped] != nullptr) {
      Tip("%s", item_tooltips[clamped]);
    } else {
      Tip("Current %s: %s.", label, preview);
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
                     float fixed_button_width) {
  if (count <= 0 || labels == nullptr) return -1;

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

    if (is_disabled) ImGui::BeginDisabled();

    const int pushed = is_selected ? 3 : 0;
    if (is_selected) {
      const ImVec4 accent =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImGui::PushStyleColor(ImGuiCol_Button, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
    }

    if (ImGui::Button(labels[i], ImVec2(button_width, 0.0f)) &&
        !is_selected && !is_disabled) {
      clicked = i;
    }

    if (pushed > 0) ImGui::PopStyleColor(pushed);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (tooltips != nullptr && tooltips[i] != nullptr) {
        Tip("%s", tooltips[i]);
      } else {
        Tip("Select %s.", labels[i]);
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
  char fallback_tooltip[192];
  if (tooltip == nullptr || tooltip[0] == '\0') {
    std::snprintf(fallback_tooltip, sizeof(fallback_tooltip),
                  "Choose the %s setting.", label);
    tooltip = fallback_tooltip;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("%s", tooltip);
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

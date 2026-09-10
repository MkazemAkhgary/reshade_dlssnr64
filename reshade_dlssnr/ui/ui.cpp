#include <imgui.h>

#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compute/shader_params.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "settings.h"
#include "version.h"

namespace reshade_dlssnr {

namespace {

constexpr float kControlLabelWidth = 220.0f;
constexpr float kResetButtonWidth = 24.0f;

// ---------------------------------------------------------------------------
// Row helpers. Every control that participates in the "label column + content"
// layout uses one of these so the columns line up and the reset-button spacing
// is consistent. Adding new controls to a section should use a helper, not
// hand-rolled SameLine() math.
// ---------------------------------------------------------------------------

// A float slider row: label at the control column, slider filling the
// remaining width, reset button at the far right. Returns true if changed.
bool DrawFloatRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  float* value,
                  float lo,
                  float hi,
                  float default_value) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kResetButtonWidth -
                          ImGui::GetStyle().ItemSpacing.x);
  const std::string slider_id = std::string("##") + id;
  changed |= ImGui::SliderFloat(slider_id.c_str(), value, lo, hi, "%.2f");
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reset to default");
  }
  ImGui::PopID();

  return changed;
}

// A checkbox row that sits in the label column, with its caption after it.
// Used for standalone checkboxes and as the left half of composite rows.
bool DrawCheckboxAtLabel(const char* id, bool* value) {
  const std::string cb_id = std::string("##") + id;
  return ImGui::Checkbox(cb_id.c_str(), value);
}

// A combo row: label at the control column, combo filling the rest.
bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  int item_count) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);
  const std::string combo_id = std::string("##") + id;
  return ImGui::Combo(combo_id.c_str(), value, items, item_count);
}

// Runs `fn` inside a disabled region when `disabled` is true.
template <typename Fn>
void WithDisabled(bool disabled, Fn&& fn) {
  if (disabled) ImGui::BeginDisabled();
  fn();
  if (disabled) ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Section: Target
// ---------------------------------------------------------------------------

void DrawTargetSection(Settings& settings, uint32_t pass_index,
                       bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  // A pass can be at DLSS Output only if every earlier pass is too.
  bool can_be_at_dlss = true;
  for (uint32_t j = 0; j < pass_index; ++j) {
    if (settings.passes[j].injection_stage == InjectionStage::Present) {
      can_be_at_dlss = false;
      break;
    }
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  ImGui::SameLine(kControlLabelWidth);

  const float button_width =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) /
      2.0f;

  const bool at_dlss = pass.injection_stage == InjectionStage::PostDLSS;
  const bool at_present = pass.injection_stage == InjectionStage::Present;

  WithDisabled(!can_be_at_dlss, [&] {
    if (at_dlss) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("DLSS Output", ImVec2(button_width, 0.0f)) &&
        can_be_at_dlss && !at_dlss) {
      pass.injection_stage = InjectionStage::PostDLSS;
      changed = true;
    }
    if (at_dlss) ImGui::PopStyleColor();
  });

  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

  if (at_present) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("Backbuffer", ImVec2(button_width, 0.0f)) && !at_present) {
    pass.injection_stage = InjectionStage::Present;
    // Once a pass targets the backbuffer, all later passes must too.
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
  }
  if (at_present) ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Section: Pipeline (Working Scale + Debug View)
// ---------------------------------------------------------------------------

void DrawPipelineSection(PassSettings& pass, bool& changed) {
  changed |= DrawFloatRow(
      "working_scale", "Working Scale",
      "Model evaluation resolution as a fraction of the frame. "
      "1.0 runs the model at full resolution.",
      &pass.working_scale, 0.25f, 1.0f, 1.0f);

  // Debug view selector. Auto-populated from the debug view registry.
  {
    std::vector<const char*> names;
    names.reserve(8);
    names.push_back("Off");
    for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
      names.push_back(kDebugViews[i].name);
    }

    changed |= DrawComboRow("debug_view", "Debug View", nullptr,
                            &pass.debug_view, names.data(),
                            static_cast<int>(names.size()));
  }
}

// ---------------------------------------------------------------------------
// Section: Model (preset, tuning, character mask)
// ---------------------------------------------------------------------------

// The Skin Structure control is a composite: checkbox + caption + slider +
// reset button, all on one row, with the slider and reset disabled when the
// checkbox is off. This needs a bespoke layout rather than DrawFloatRow.
bool DrawSkinStructureRow(PassSettings& pass) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("skin_structure_enabled", &pass.auto_mask);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Enables the model's character skin mask and adjusts its strength.");
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("Skin Structure");
  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - kResetButtonWidth -
                          ImGui::GetStyle().ItemSpacing.x);

  WithDisabled(!pass.auto_mask, [&] {
    changed |= ImGui::SliderFloat("##skin_structure_value",
                                  &pass.skin_structure, 0.0f, 1.0f, "%.2f");
  });

  ImGui::SameLine();
  ImGui::PushID("skin_structure_reset");
  WithDisabled(!pass.auto_mask, [&] {
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
  });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
  ImGui::PopID();

  return changed;
}

void DrawModelSection(PassSettings& pass, bool& changed) {
  if (!ImGui::TreeNodeEx("Model",
                         ImGuiTreeNodeFlags_CollapsingHeader |
                             ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  // Preset row: three buttons of equal width on the content column.
  {
    const float button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 3.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine(kControlLabelWidth);

    static const char* kPresetNames[] = {"A", "B", "C"};
    for (int i = 0; i < 3; ++i) {
      if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

      const bool selected = pass.preset == i;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(kPresetNames[i], ImVec2(button_width, 0.0f)) &&
          pass.preset != i) {
        pass.preset = i;
        changed = true;
      }
      if (selected) ImGui::PopStyleColor();
    }
  }

  changed |= DrawFloatRow("intensity", "Intensity", nullptr, &pass.intensity,
                          0.0f, 2.0f, 1.0f);
  changed |= DrawFloatRow("local_tone", "Local Tone", nullptr, &pass.local_tone,
                          0.0f, 2.0f, 1.0f);
  changed |= DrawFloatRow("local_structure", "Local Structure", nullptr,
                          &pass.local_structure, 0.0f, 2.0f, 1.0f);

  changed |= DrawSkinStructureRow(pass);
}

// ---------------------------------------------------------------------------
// Section: Dynamic shader parameters (one collapsible per category)
// ---------------------------------------------------------------------------

void DrawShaderParamSections(PassSettings& pass, bool& changed) {
  uint32_t i = 0;
  while (i < kShaderParamCount) {
    const char* category = kShaderParams[i].category;

    uint32_t next = i;
    while (next < kShaderParamCount &&
           strcmp(kShaderParams[next].category, category) == 0) {
      ++next;
    }

    ImGui::Separator();
    const bool open = ImGui::TreeNodeEx(
        category, ImGuiTreeNodeFlags_CollapsingHeader |
                      ImGuiTreeNodeFlags_DefaultOpen);

    if (open) {
      for (uint32_t j = i; j < next; ++j) {
        const auto& param = kShaderParams[j];
        const std::string id = std::string(category) + "_" + param.name;

        if (param.is_checkbox) {
          bool checked = pass.params[j] > 0.5f;
          ImGui::AlignTextToFramePadding();
          changed |= DrawCheckboxAtLabel(id.c_str(), &checked);
          if (ImGui::IsItemHovered() && param.tooltip != nullptr) {
            ImGui::SetTooltip("%s", param.tooltip);
          }
          ImGui::SameLine();
          ImGui::TextUnformatted(param.name);
          pass.params[j] = checked ? 1.0f : 0.0f;
        } else {
          changed |= DrawFloatRow(id.c_str(), param.name, param.tooltip,
                                  &pass.params[j], param.min_value,
                                  param.max_value, param.default_value);
        }
      }
    }

    i = next;
  }
}

// ---------------------------------------------------------------------------
// Section: Advanced
// ---------------------------------------------------------------------------

void DrawAdvancedSection(PassSettings& pass, bool& changed) {
  if (!ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_CollapsingHeader)) {
    return;
  }

  ImGui::AlignTextToFramePadding();
  changed |= ImGui::Checkbox("UI Correction", &pass.ui_correction);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Experimental. Enables the model's UI correction pass.");
  }
}

// ---------------------------------------------------------------------------
// Pass tab body
// ---------------------------------------------------------------------------

void DrawPassSettingsBody(uint32_t pass_index,
                          Settings& settings,
                          bool enabled,
                          bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  ImGui::PushID(static_cast<int>(pass_index));
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass, changed);
    ImGui::Separator();
    DrawModelSection(pass, changed);
    DrawShaderParamSections(pass, changed);
    ImGui::Separator();
    DrawAdvancedSection(pass, changed);
  });
  ImGui::PopID();
}

}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  bool changed = false;

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Require DLSS", &settings.only_super_sampling);

  if (settings.enabled) {
    for (uint32_t i = 1; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Enable Pass %u", i + 1);
      changed |= ImGui::Checkbox(label, &settings.pass_enabled[i]);
    }
  }

  ImGui::Separator();

  if (ImGui::BeginTabBar("PassTabs")) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Pass %u", i + 1);
      if (ImGui::BeginTabItem(label)) {
        // The open tab determines which pass's debug view is rendered.
        settings.active_pass = static_cast<int>(i);
        DrawPassSettingsBody(i, settings, settings.pass_enabled[i], changed);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

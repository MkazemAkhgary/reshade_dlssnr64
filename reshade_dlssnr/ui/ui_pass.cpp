#include <imgui.h>
#include <reshade.hpp>

#include <cstdio>
#include <cstring>
#include <string>

#include "gpu/shader_params.h"
#include "state/settings.h"
#include "grade/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

PendingMove g_pending_move = PendingMove::None;
int g_pending_move_index = -1;

namespace {

void DrawTargetSection(Settings& settings, uint32_t pass_index,
                       bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  static const char* kTargetLabels[] = {"Pre-DLSS", "Post-DLSS", "Backbuffer"};
  static const char* kTargetTips[] = {
      "Run before the game's DLSS evaluation on its input Color resource. "
      "The game then upscales the modified image. Requires a full-resource Color input.",
      "Run after the game's DLSS evaluation on its upscaled output, before "
      "later post-processing and UI.",
      "Run on the final swapchain backbuffer, after DLSS-stage passes."};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Each pass can independently target Pre-DLSS, Post-DLSS, or Backbuffer. "
        "Execution order is grouped by stage; passes within each stage follow "
        "their pass-tab order.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const int selected = static_cast<int>(pass.injection_stage);
  const int clicked = DrawGroupButtons("target", kTargetLabels, kTargetTips,
                                       3, selected, nullptr);
  if (clicked < 0 || clicked > 2 || clicked == selected) return;

  pass.injection_stage = static_cast<InjectionStage>(clicked);
  changed = true;
}

void DrawPipelineSection(uint32_t pass_index, Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  changed |= DrawFloatRow(
      pass_index, "working_scale", "Working Scale",
      "The resolution the model runs at, as a fraction of the frame.",
      &pass.working_scale, &pass.inherit_working_scale,
      resolved.working_scale, 0.10f, 1.0f, 1.0f);
}

bool DrawSkinStructureRow(uint32_t pass_index, PassSettings& pass,
                          const PassSettings& resolved) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("skin_structure_enabled", &pass.auto_mask);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Lets the model detect character skin and apply its edits to it "
        "separately.");
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("Skin Structure");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Lets the model detect character skin and apply its edits to it separately.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = pass_index > 0;
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - trailing_width -
                          ImGui::GetStyle().ItemSpacing.x);

  const bool inheriting = has_inherit && pass.inherit_skin_structure;
  const bool slider_disabled = !pass.auto_mask || inheriting;
  float display_value =
      inheriting ? resolved.skin_structure : pass.skin_structure;

  WithDisabled(slider_disabled, [&] {
    if (ImGui::SliderFloat("##skin_structure_value", &display_value,
                           0.0f, 1.0f, "%.2f") && !inheriting) {
      pass.skin_structure = display_value;
      changed = true;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Controls how strongly the model applies its skin-specific edits. Disabled when the model's automatic masking option is off or this value is inherited.");
  }

  ImGui::SameLine();
  ImGui::PushID("skin_structure_reset");
  WithDisabled(!pass.auto_mask, [&] {
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset Skin Structure strength to 1.00.");
  }
  ImGui::PopID();

  if (has_inherit) {
    ImGui::SameLine();
    ImGui::PushID("skin_structure_inherit");
    const bool on = pass.inherit_skin_structure;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.inherit_skin_structure = !pass.inherit_skin_structure;
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Inherit Skin Structure from the previous pass instead of using a local value.");
    }
    if (on) ImGui::PopStyleColor();
    ImGui::PopID();
  }

  return changed;
}

void DrawModelSection(uint32_t pass_index, Settings& settings,
                      bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  IndentScope indent;
  if (!PersistentSubHeader("Model", settings.ui_model_open, changed)) return;

  {
    static const char* kPresetNames[] = {"A", "B", "C"};
    static const char* kPresetTooltips[] = {
        "Use model preset A. Preset changes are applied per evaluation and do not rebuild the feature.",
        "Use model preset B. Preset changes are applied per evaluation and do not rebuild the feature.",
        "Use model preset C. Preset changes are applied per evaluation and do not rebuild the feature."};

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Select the model quality/behavior preset for this pass.");
    }
    ImGui::SameLine(kControlLabelWidth);

    const int clicked = DrawGroupButtons(
        "preset", kPresetNames, kPresetTooltips, 3, pass.preset, nullptr,
        60.0f);
    if (clicked >= 0) {
      pass.preset = clicked;
      changed = true;
    }
  }

  changed |= DrawFloatRow(
      pass_index, "intensity", "Intensity",
      "How strongly the model is instructed to edit the image. 0 turns "
      "the model's edits off entirely.",
      &pass.intensity, &pass.inherit_intensity, resolved.intensity,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_tone", "Local Tone",
      "How much of the model's local tone adjustments to apply.",
      &pass.local_tone, &pass.inherit_local_tone, resolved.local_tone,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_structure", "Local Structure",
      "How much of the model's local structure detail to apply.",
      &pass.local_structure, &pass.inherit_local_structure,
      resolved.local_structure, 0.0f, 2.0f, 1.0f);

  changed |= DrawSkinStructureRow(pass_index, pass, resolved);

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("ui_correction", &pass.ui_correction);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reduce the model's edits where it detects UI elements.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reduce the model's edits where it detects UI elements.");
  }
}

int FindShaderParamIndex(const char* name) {
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    if (strcmp(kShaderParams[i].name, name) == 0) return static_cast<int>(i);
  }
  return -1;
}

void DrawShaderParamSections(uint32_t pass_index, Settings& settings,
                             bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  uint32_t i = 0;
  uint32_t category_index = 0;
  while (i < kShaderParamCount) {
    const char* category = kShaderParams[i].category;

    uint32_t next = i;
    while (next < kShaderParamCount &&
           strcmp(kShaderParams[next].category, category) == 0) {
      ++next;
    }

    ImGui::Separator();
    bool& category_open =
        settings.ui_category_open[category_index < 16 ? category_index : 15];

    IndentScope indent;
    const bool open = PersistentSubHeader(category, category_open, changed);
    category_index++;

    if (open) {
      for (uint32_t j = i; j < next; ++j) {
        const auto& param = kShaderParams[j];
        const std::string id = std::string(category) + "_" + param.name;

        bool dependency_off = false;
        if (param.depends_on != nullptr) {
          const int gate = FindShaderParamIndex(param.depends_on);
          if (gate >= 0 && pass.params[gate] < 0.5f) dependency_off = true;
        }

        WithDisabled(dependency_off, [&] {
          if (param.is_checkbox) {
            bool checked = pass.params[j] > 0.5f;
            ImGui::AlignTextToFramePadding();
            changed |= DrawCheckboxAtLabel(id.c_str(), &checked);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
              Tip("%s", param.tooltip != nullptr
                              ? param.tooltip
                              : "Toggle this shader parameter.");
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(param.name);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
              Tip("%s", param.tooltip != nullptr
                              ? param.tooltip
                              : "Toggle this shader parameter.");
            }
            pass.params[j] = checked ? 1.0f : 0.0f;
          } else {
            changed |= DrawFloatRow(
                pass_index, id.c_str(), param.name, param.tooltip,
                &pass.params[j], &pass.inherit_params[j], resolved.params[j],
                param.min_value, param.max_value, param.default_value);
          }
        });
      }
    }

    i = next;
  }
}

void DrawResidualSection(uint32_t pass_index, Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Residual Edit", settings.ui_residual_open,
                           changed)) {
    return;
  }

  changed |= DrawFloatRow(
      pass_index, "edit_strength", "Edit Strength",
      "Scales the model's edit before it is carried onto the source.",
      &pass.residual.edit_strength, nullptr, 0.0f, 0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_chroma", "Chroma Strength",
      "How much of the edit's chroma lands relative to its brightness.",
      &pass.residual.chroma_strength, nullptr, 1.0f, 0.0f, 1.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_gamma", "Edit Gamma",
      "Shapes the edit's response curve.",
      &pass.residual.edit_gamma, nullptr, 1.0f, 0.25f, 4.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_highlight", "Highlight Protect",
      "Fades the edit out above mid-to-high brightness.",
      &pass.residual.highlight_protect, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_shadow", "Shadow Protect",
      "Fades the edit out below mid-to-low brightness.",
      &pass.residual.shadow_protect, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_eps", "Ratio Eps",
      "Denominator floor for the per-channel ratio. Ignored by Gain-Map Residual mode.",
      &pass.residual.ratio_eps, nullptr, 1e-2f, 1e-6f, 0.5f, 1e-2f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_brighten", "Ratio Brighten",
      "Upper bound on ratio brightening. Soft-clipped; ignored by Gain-Map Residual mode.",
      &pass.residual.ratio_brighten, nullptr, 64.0f, 1.0f, 256.0f, 64.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_darken", "Ratio Darken",
      "Lower bound on ratio darkening. Soft-clipped; ignored by Gain-Map Residual mode.",
      &pass.residual.ratio_darken, nullptr, 1.0f / 16.0f,
      1.0f / 256.0f, 1.0f, 1.0f / 16.0f);
}

void ResetPassProfile(uint32_t pass_index, Settings& settings) {
  if (pass_index >= kMaxPasses) return;

  PassSettings& pass = settings.passes[pass_index];
  const InjectionStage target = pass.injection_stage;
  char name[sizeof(pass.custom_name)] = {};
  std::snprintf(name, sizeof(name), "%s", pass.custom_name);

  PassSettings defaults{};
  defaults.injection_stage = target;
  std::snprintf(defaults.custom_name, sizeof(defaults.custom_name), "%s", name);
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    defaults.params[i] = kShaderParams[i].default_value;
  }
  pass = defaults;
}

void DrawRcasSection(uint32_t pass_index, Settings& settings, bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Sharpening", settings.ui_rcas_open, changed)) {
    return;
  }

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("rcas_enabled", &pass.rcas_enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Sharpen the model's upsampled output with FSR RCAS.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Enable RCAS");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Sharpen the model's upsampled output with FSR RCAS.");
  }

  WithDisabled(!pass.rcas_enabled, [&] {
    changed |= DrawFloatRow(
        pass_index, "rcas_strength", "Strength",
        "How aggressively to sharpen.",
        &pass.rcas_strength, nullptr, 0.5f, 0.0f, 1.0f, 0.5f);
  });
}

}  // namespace

void DrawPassSettingsBody(uint32_t pass_index, Settings& settings,
                          bool& changed) {
  ImGui::PushID(static_cast<int>(pass_index));

  PassSettings& pass = settings.passes[pass_index];

  char label[32];
  std::snprintf(label, sizeof(label), "Enable Pass %u", pass_index + 1);
  changed |= ImGui::Checkbox(label, &settings.pass_enabled[pass_index]);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Enable or disable this pass profile. Disabled passes do not evaluate the model.");
  }

  ImGui::SameLine();
  const bool can_move_left = pass_index > 0;
  WithDisabled(!can_move_left, [&] {
    if (ImGui::Button("<", ImVec2(30, 0)) && can_move_left) {
      g_pending_move = PendingMove::Left;
      g_pending_move_index = static_cast<int>(pass_index);
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move this pass one position earlier. Execution still groups by target stage.");
  }

  ImGui::SameLine();
  const bool can_move_right = pass_index + 1 < kMaxPasses;
  WithDisabled(!can_move_right, [&] {
    if (ImGui::Button(">", ImVec2(30, 0)) && can_move_right) {
      g_pending_move = PendingMove::Right;
      g_pending_move_index = static_cast<int>(pass_index);
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move this pass one position later. Execution still groups by target stage.");
  }

  ImGui::SameLine();
  if (ImGui::Button("Reset Pass")) {
    ResetPassProfile(pass_index, settings);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset this pass's model, residual, grade, sharpening, shader, and inheritance settings to defaults. Its name, target, and enabled state are preserved.");
  }

  ImGui::Separator();

  {
    char name_buffer[64];
    std::snprintf(name_buffer, sizeof(name_buffer), "%s", pass.custom_name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Optional label shown on the pass tab. Leave blank to display the default Pass N name.");
    }
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                            kResetButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##pass_name", name_buffer, sizeof(name_buffer))) {
      std::snprintf(pass.custom_name, sizeof(pass.custom_name), "%s",
                    name_buffer);
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Set a custom display name for this pass. This does not change pass ordering or settings identity.");
    }
    ImGui::SameLine();
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.custom_name[0] = '\0';
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Clear the custom name and use the default Pass N label.");
    }
  }

  const bool enabled = settings.pass_enabled[pass_index];
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass_index, settings, changed);
    ImGui::Separator();

    DrawModelSection(pass_index, settings, changed);

    {
      IndentScope advanced_indent;
      if (PersistentSubHeader(
              "Advanced", settings.ui_pass_advanced_open, changed,
              "Fine-tune shader parameters and how the model's residual edit is applied.")) {
        DrawShaderParamSections(pass_index, settings, changed);
        DrawResidualSection(pass_index, settings, changed);
      }
    }

    DrawRcasSection(pass_index, settings, changed);

    {
      IndentScope grade_indent;
      if (PersistentSubHeader(
              "Grade", settings.ui_pass_grade_open, changed,
              "Optionally grade the model input and output independently.")) {
        IndentScope grade_children_indent;
        if (PersistentSubHeader(
                "Model Input", settings.ui_grade_input_open, changed,
                "Grade the image before the model reads it. This is independent of the output and backbuffer grades.")) {
          DrawGradeSection(
              "grade_input", "Enabled",
              "Grade the image the model reads, before model evaluation.",
              settings.passes[pass_index].grade_input, changed);
        }
        if (PersistentSubHeader(
                "Model Output", settings.ui_grade_output_open, changed,
                "Grade the model's output before it is composed and published. This is independent of the input and backbuffer grades.")) {
          DrawGradeSection(
              "grade_output", "Enabled",
              "Grade the image the model produced, before it is published.",
              settings.passes[pass_index].grade_output, changed);
        }
      }
    }
  });
  ImGui::PopID();
}

}  // namespace reshade_dlssnr

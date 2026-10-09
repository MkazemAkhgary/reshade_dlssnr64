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

  bool can_be_at_dlss = true;
  for (uint32_t j = 0; j < pass_index; ++j) {
    if (settings.passes[j].injection_stage == InjectionStage::Present) {
      can_be_at_dlss = false;
      break;
    }
  }

  static const char* kTargetLabels[] = {"DLSS Output", "Backbuffer"};
  static const char* kTargetTips[] = {
      "Run this pass on the DLSS upscaled image, before the game applies "
      "UI and tone mapping.\n"
      "Unavailable when an earlier pass targets the Backbuffer, because "
      "passes run in order and later stages cannot reach back to earlier "
      "ones.",
      "Run this pass on the final frame, after every other effect "
      "including UI and post-processing.\n"
      "Selecting this forces all later passes to Backbuffer as well."};
  const bool kTargetDisabled[] = {!can_be_at_dlss, false};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  if (ImGui::IsItemHovered()) {
    Tip("Where this pass runs in the frame.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const int selected =
      pass.injection_stage == InjectionStage::PostDLSS ? 0 : 1;
  const int clicked = DrawGroupButtons("target", kTargetLabels, kTargetTips,
                                       2, selected, kTargetDisabled);

  if (clicked == 0) {
    pass.injection_stage = InjectionStage::PostDLSS;
    changed = true;
  } else if (clicked == 1) {
    pass.injection_stage = InjectionStage::Present;
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
  }
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
  if (ImGui::IsItemHovered()) {
    Tip("Lets the model detect character skin and apply its edits to it "
        "separately.");
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("Skin Structure");
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

  ImGui::SameLine();
  ImGui::PushID("skin_structure_reset");
  WithDisabled(!pass.auto_mask, [&] {
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
  });
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
        "Model preset A.", "Model preset B.", "Model preset C."};

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine(kControlLabelWidth);

    const int clicked = DrawGroupButtons(
        "preset", kPresetNames, kPresetTooltips, 3, pass.preset, nullptr);
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
  if (ImGui::IsItemHovered()) {
    Tip("Reduce the model's edits where it detects UI elements.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
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
            if (ImGui::IsItemHovered() && param.tooltip != nullptr) {
              Tip("%s", param.tooltip);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(param.name);
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

  {
    static const char* const kHdrComposeLabels[] = {
        "Ratio (Current)",
        "Gain-Map Residual (Scene-Referred)"};
    static const char* const kHdrComposeTooltips[] = {
        "Transfers a bounded per-channel relative edit in the linear model domain onto the original HDR source.",
        "Transfers the model-domain relative edit through the stored per-pixel reciprocal multiplier, preserving the original HDR highlight range."};
    int mode = static_cast<int>(pass.residual.hdr_compose_mode);
    if (DrawComboRow("hdr_compose_mode", "HDR Composition",
                     "Select how the model edit is carried back into HDR. Both modes preserve the original scene-referred source; Gain-Map Residual uses the per-pixel reciprocal multiplier and avoids inverting the nonlinear highlight fit at composition time.",
                     &mode, kHdrComposeLabels, kHdrComposeTooltips, 2)) {
      pass.residual.hdr_compose_mode = static_cast<HdrComposeMode>(mode);
      changed = true;
    }
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

void DrawRcasSection(uint32_t pass_index, Settings& settings, bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Sharpening", settings.ui_rcas_open, changed)) {
    return;
  }

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("rcas_enabled", &pass.rcas_enabled);
  if (ImGui::IsItemHovered()) {
    Tip("Sharpen the model's upsampled output with FSR RCAS.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Enable RCAS");

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
  ImGui::Separator();

  {
    char name_buffer[64];
    std::snprintf(name_buffer, sizeof(name_buffer), "%s", pass.custom_name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                            kResetButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##pass_name", name_buffer, sizeof(name_buffer))) {
      std::snprintf(pass.custom_name, sizeof(pass.custom_name), "%s",
                    name_buffer);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.custom_name[0] = '\0';
      changed = true;
    }
  }

  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Order");
    ImGui::SameLine(kControlLabelWidth);

    const bool can_move_left = pass_index > 0;
    const bool can_move_right = pass_index + 1 < kMaxPasses;

    WithDisabled(!can_move_left, [&] {
      if (ImGui::Button("<", ImVec2(30, 0)) && can_move_left) {
        g_pending_move = PendingMove::Left;
        g_pending_move_index = static_cast<int>(pass_index);
      }
    });

    ImGui::SameLine();

    WithDisabled(!can_move_right, [&] {
      if (ImGui::Button(">", ImVec2(30, 0)) && can_move_right) {
        g_pending_move = PendingMove::Right;
        g_pending_move_index = static_cast<int>(pass_index);
      }
    });
  }

  const bool enabled = settings.pass_enabled[pass_index];
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawModelSection(pass_index, settings, changed);
    DrawShaderParamSections(pass_index, settings, changed);
    DrawResidualSection(pass_index, settings, changed);
    DrawRcasSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawGradeSection(
        "grade_input", "Grade Model Input",
        "Grade the image the model reads.",
        settings.passes[pass_index].grade_input, changed);
    DrawGradeSection(
        "grade_output", "Grade Model Output",
        "Grade the image the model produced, before it is published.",
        settings.passes[pass_index].grade_output, changed);
  });
  ImGui::PopID();
}

}  // namespace reshade_dlssnr

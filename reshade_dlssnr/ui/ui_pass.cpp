#include <imgui.h>
#include <reshade.hpp>

#include <cstdio>
#include <cstring>

#include "state/settings.h"
#include "grade/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

PendingMove g_pending_move = PendingMove::None;
int g_pending_move_index = -1;
PendingPassAction g_pending_pass_action = PendingPassAction::None;

namespace {

void ResetModelSection(void* context) {
  auto& pass = *static_cast<PassSettings*>(context);
  const PassSettings defaults{};
  pass.preset = defaults.preset;
  pass.intensity = defaults.intensity;
  pass.local_tone = defaults.local_tone;
  pass.ui_correction = defaults.ui_correction;
  pass.local_structure = defaults.local_structure;
  pass.auto_mask = defaults.auto_mask;
  pass.skin_structure = defaults.skin_structure;
  pass.inherit_intensity = false;
  pass.inherit_local_tone = false;
  pass.inherit_local_structure = false;
  pass.inherit_skin_structure = false;
}

void ResetStrengthSection(void* context) {
  auto& pass = *static_cast<PassSettings*>(context);
  const PassSettings defaults{};
  pass.strength = defaults.strength;
  pass.inherit_luma_strength = false;
  pass.inherit_chroma_strength = false;
}

void ResetSoftCompressionSection(void* context) {
  auto& pass = *static_cast<PassSettings*>(context);
  const PassSettings defaults{};
  pass.soft_compression = defaults.soft_compression;
  pass.inherit_soft_max_brighten = false;
  pass.inherit_soft_max_darken = false;
  pass.inherit_soft_max_chroma = false;
  pass.inherit_soft_knee = false;
}

void ResetResidualSection(void* context) {
  static_cast<PassSettings*>(context)->residual = ResidualSettings{};
}

void ResetRcasSection(void* context) {
  auto& pass = *static_cast<PassSettings*>(context);
  pass.rcas_enabled = false;
  pass.rcas_strength = 0.5f;
}

void ResetAdvancedSection(void* context) {
  auto& pass = *static_cast<PassSettings*>(context);
  ResetSoftCompressionSection(context);
  pass.residual = ResidualSettings{};
  pass.rcas_enabled = false;
  pass.rcas_strength = 0.5f;
}

void ResetGradeSettings(void* context) {
  *static_cast<GradeSettings*>(context) = GradeSettings{};
}

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
  ImGui::SameLine(kSliderLabelWidth);

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

  if (!PersistentSubHeader(
          "Model", settings.ui_model_open, changed, nullptr,
          HeaderControls{nullptr, nullptr, false, ResetModelSection, &pass})) {
    return;
  }
  IndentScope content_indent;

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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reduce the model's edits where it detects UI elements.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reduce the model's edits where it detects UI elements.");
  }
}

void DrawStrengthSection(uint32_t pass_index, Settings& settings,
                        bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  if (!PersistentSubHeader(
          "Strength", settings.ui_strength_open, changed,
          "Blend of the model's luminance and chroma edits into the source when the model output is composed back.",
          HeaderControls{nullptr, nullptr, false, ResetStrengthSection, &pass})) {
    return;
  }
  IndentScope content_indent;

  changed |= DrawFloatRow(
      pass_index, "luma_strength", "Luma Strength",
      "Blend between original and edited luminance.",
      &pass.strength.luma_strength, &pass.inherit_luma_strength,
      resolved.strength.luma_strength, 0.0f, 1.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "chroma_strength", "Chroma Strength",
      "Blend between original and edited chroma.",
      &pass.strength.chroma_strength, &pass.inherit_chroma_strength,
      resolved.strength.chroma_strength, 0.0f, 1.0f, 1.0f);
}

void DrawSoftCompressionSection(uint32_t pass_index, Settings& settings,
                               bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  if (!PersistentSubHeader(
          "Soft Compression", settings.ui_soft_compression_open, changed,
          "Enable rational soft clamp of log residuals.",
          HeaderControls{&pass.soft_compression.enabled,
                         "Enable rational soft clamp of log residuals.",
                         false, ResetSoftCompressionSection, &pass})) {
    return;
  }
  IndentScope content_indent;

  WithDisabled(!pass.soft_compression.enabled, [&] {
    changed |= DrawFloatRow(
        pass_index, "soft_max_brighten", "Max Brighten",
        "Maximum allowed log-luma brightening.",
        &pass.soft_compression.max_brighten,
        &pass.inherit_soft_max_brighten,
        resolved.soft_compression.max_brighten, 0.0f, 2.0f, 0.5f);

    changed |= DrawFloatRow(
        pass_index, "soft_max_darken", "Max Darken",
        "Maximum allowed log-luma darkening.",
        &pass.soft_compression.max_darken,
        &pass.inherit_soft_max_darken,
        resolved.soft_compression.max_darken, 0.0f, 2.0f, 0.5f);

    changed |= DrawFloatRow(
        pass_index, "soft_max_chroma", "Max Chroma",
        "Maximum allowed log-chroma change per channel.",
        &pass.soft_compression.max_chroma,
        &pass.inherit_soft_max_chroma,
        resolved.soft_compression.max_chroma, 0.0f, 2.0f, 0.5f);

    changed |= DrawFloatRow(
        pass_index, "soft_knee", "Knee",
        "Controls how early compression starts (linear range).",
        &pass.soft_compression.knee, &pass.inherit_soft_knee,
        resolved.soft_compression.knee, 0.0f, 4.0f, 1.0f);
  });
}

void DrawResidualSection(uint32_t pass_index, Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  if (!PersistentSubHeader(
          "Residual Edit", settings.ui_residual_open, changed, nullptr,
          HeaderControls{nullptr, nullptr, false, ResetResidualSection, &pass})) {
    return;
  }
  IndentScope content_indent;

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

  {
    static const char* kUpsampleLabels[] = {"EASU", "Joint Bilateral"};
    static const char* kUpsampleTips[] = {
        "FSR 1.0 EASU upsampling. The original upsampler, designed for "
        "roughly 2x reconstruction. At working scales far from that it "
        "has less source detail to work with and the composed image "
        "can look blocky.",
        "Joint bilateral upsampling guided by the full-resolution "
        "original. Each output pixel is a weighted average of the "
        "nearby low-resolution samples, weighted by guide-luma "
        "similarity. Preserves the model's per-channel edit at any "
        "working scale."};
    int upsample_mode = static_cast<int>(pass.residual.upsample_mode);
    if (DrawComboRow("residual_upsample_mode", "Upsample Mode",
                     "Which algorithm reconstructs full resolution from "
                     "the working-scale model input and output. EASU is "
                     "the original FSR 1.0 path; Joint Bilateral uses "
                     "the full-resolution original as an edge guide.",
                     &upsample_mode, kUpsampleLabels, kUpsampleTips, 2)) {
      pass.residual.upsample_mode = static_cast<UpsampleMode>(upsample_mode);
      changed = true;
    }
  }

  WithDisabled(pass.residual.upsample_mode !=
                   UpsampleMode::JointBilateral, [&] {
    changed |= DrawFloatRow(
        pass_index, "residual_upsample_sharpness", "Upsample Sharpness",
        "Edge-preservation exponent for the guided upsample of the "
        "model's input and output. Higher values track the "
        "full-resolution image's edges more sharply; lower values "
        "blend more smoothly across the low-resolution neighborhood. "
        "This is an edge-preservation exponent, not a strength "
        "multiplier: it does not amplify or attenuate the model's edit.",
        &pass.residual.upsample_sharpness, nullptr, 8.0f, 0.5f, 32.0f, 8.0f);
  });

  const bool ratio_controls_disabled =
      pass.injection_stage != InjectionStage::Backbuffer;
  WithDisabled(ratio_controls_disabled, [&] {
    changed |= DrawFloatRow(
        pass_index, "residual_ratio_eps", "Ratio Eps",
        "Backbuffer passes only: denominator floor for the per-channel ratio. This has no effect on Pre-DLSS or Post-DLSS passes.",
        &pass.residual.ratio_eps, nullptr, 1e-2f, 1e-6f, 0.5f, 1e-2f);

    changed |= DrawFloatRow(
        pass_index, "residual_ratio_brighten", "Ratio Brighten",
        "Backbuffer passes only: upper bound on per-channel ratio brightening. This has no effect on Pre-DLSS or Post-DLSS passes.",
        &pass.residual.ratio_brighten, nullptr, 64.0f, 1.0f, 256.0f, 64.0f);

    changed |= DrawFloatRow(
        pass_index, "residual_ratio_darken", "Ratio Darken",
        "Backbuffer passes only: lower bound on per-channel ratio darkening. This has no effect on Pre-DLSS or Post-DLSS passes.",
        &pass.residual.ratio_darken, nullptr, 1.0f / 16.0f,
        1.0f / 256.0f, 1.0f, 1.0f / 16.0f);
  });
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
  pass = defaults;
}

void DrawRcasSection(uint32_t pass_index, Settings& settings, bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  if (!PersistentSubHeader(
          "Sharpening", settings.ui_rcas_open, changed,
          "Sharpen the model's upsampled output with FSR RCAS.",
          HeaderControls{&pass.rcas_enabled,
                         "Sharpen the model's upsampled output with FSR RCAS.",
                         false, ResetRcasSection, &pass})) {
    return;
  }
  IndentScope content_indent;

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
    if (ImGui::Button("<", ImVec2(kResetButtonWidth, 0)) && can_move_left) {
      g_pending_move = PendingMove::Left;
      g_pending_move_index = static_cast<int>(pass_index);
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move this pass one position earlier. Execution still groups by target stage.");
  }

  ImGui::SameLine();
  const bool can_move_right = pass_index + 1 < settings.pass_count;
  WithDisabled(!can_move_right, [&] {
    if (ImGui::Button(">", ImVec2(kResetButtonWidth, 0)) && can_move_right) {
      g_pending_move = PendingMove::Right;
      g_pending_move_index = static_cast<int>(pass_index);
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move this pass one position later. Execution still groups by target stage.");
  }

  ImGui::SameLine();
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0))) {
    ResetPassProfile(pass_index, settings);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset this pass's model, residual, grade, sharpening, shader, and inheritance settings to defaults. Its name, target, and enabled state are preserved.");
  }

  ImGui::SameLine();
  const bool can_add_pass = settings.pass_count < kMaxPasses;
  bool add_clicked = false;
  WithDisabled(!can_add_pass, [&] {
    add_clicked = ImGui::Button("+", ImVec2(kResetButtonWidth, 0));
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Add a new pass, up to the maximum of %u.", kMaxPasses);
  }

  ImGui::SameLine();
  const bool can_remove_pass = settings.pass_count > 1;
  bool remove_clicked = false;
  WithDisabled(!can_remove_pass, [&] {
    remove_clicked = ImGui::Button("x", ImVec2(kResetButtonWidth, 0));
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Remove the selected pass. At least one pass remains.");
  }
  if (add_clicked || remove_clicked) {
    // Defer array mutation until the next frame, outside the active tab loop.
    g_pending_pass_action = add_clicked ? PendingPassAction::Add
                                         : PendingPassAction::Remove;
    ImGui::PopID();
    return;
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

    DrawStrengthSection(pass_index, settings, changed);

    if (PersistentSubHeader(
            "Advanced", settings.ui_pass_advanced_open, changed,
            "Fine-tune shader parameters and how the model's residual edit is applied.",
            HeaderControls{nullptr, nullptr, false, ResetAdvancedSection, &pass})) {
      IndentScope advanced_content_indent;
      DrawSoftCompressionSection(pass_index, settings, changed);
      DrawResidualSection(pass_index, settings, changed);
      DrawRcasSection(pass_index, settings, changed);
    }

    if (PersistentSubHeader(
            "Grade Model Input", settings.ui_grade_input_open, changed,
            "Grade the image before the model reads it. This is independent of the output and backbuffer grades.",
            HeaderControls{&pass.grade_input.enabled,
                           "Grade the image the model reads, before model evaluation.",
                           false, ResetGradeSettings, &pass.grade_input})) {
      IndentScope input_grade_content_indent;
      DrawGradeSection("grade_input", pass.grade_input, changed);
    }

    if (PersistentSubHeader(
            "Grade Model Output", settings.ui_grade_output_open, changed,
            "Grade the model's output before it is composed and published. This is independent of the input and backbuffer grades.",
            HeaderControls{&pass.grade_output.enabled,
                           "Grade the model produced, before it is published.",
                           false, ResetGradeSettings, &pass.grade_output})) {
      IndentScope output_grade_content_indent;
      DrawGradeSection("grade_output", pass.grade_output, changed);
    }
  });
  ImGui::PopID();
}

}  // namespace reshade_dlssnr

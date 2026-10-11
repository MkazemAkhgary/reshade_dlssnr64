#include <imgui.h>
#include <reshade.hpp>

#include "state/settings.h"
#include "skin/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

namespace {

void DrawModelGroup(SkinSettings& detection, bool& model_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Model", model_open, changed)) return;

  static const char* kNames[] = {"A", "B", "C"};
  static const char* kTips[] = {"Preset A.", "Preset B.", "Preset C."};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Preset");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Select the model preset used to generate the Structure Detection mask.");
  }
  ImGui::SameLine(kControlLabelWidth);
  const int clicked = DrawGroupButtons("structure_detect_preset", kNames,
                                       kTips, 3, detection.preset, nullptr,
                                       60.0f);
  if (clicked >= 0) {
    detection.preset = clicked;
    changed = true;
  }

  changed |= DrawFloatRow(0, "structure_detect_intensity", "Intensity", nullptr,
                          &detection.intensity, nullptr, 1.0f, 0.0f, 2.0f, 1.0f);
  changed |= DrawFloatRow(0, "structure_detect_local_tone", "Local Tone", nullptr,
                          &detection.local_tone, nullptr, 0.0f, 0.0f, 2.0f, 0.0f);
  changed |= DrawFloatRow(0, "structure_detect_local_structure", "Local Structure",
                          nullptr, &detection.local_structure, nullptr, 0.0f,
                          0.0f, 2.0f, 0.0f);
  changed |= DrawFloatRow(0, "structure_detect_target_structure", "Target Structure",
                          nullptr, &detection.skin_structure, nullptr, 1.0f,
                          0.0f, 1.0f, 1.0f);

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("structure_detect_ui_correction",
                                 &detection.ui_correction,
                                 "Reduce structure-mask edits where UI elements are detected.");
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reduce the structure detector's edits where UI elements are detected.");
  }
}

void DrawTargetGroup(SkinSettings& detection, bool& changed) {
  static const char* kNames[] = {"Skin Range", "All Structure"};
  static const char* kTips[] = {
      "Only accept model edits where every input/output channel is away from near-black and clipped-white values. This is a range gate, not a skin-hue classifier.",
      "Accept model edits at any pixel, including dark and clipped highlights. This can include faces, clothing, geometry, and other structure.",
  };
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Detection Target");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Choose whether the persistent mask accepts skin-range-like edits only or all structural edits.");
  }
  ImGui::SameLine(kControlLabelWidth);
  const int current = static_cast<int>(detection.target_mode);
  const int clicked = DrawGroupButtons("structure_detect_target", kNames,
                                       kTips, 2, current, nullptr);
  if (clicked >= 0 && clicked != current) {
    detection.target_mode = static_cast<DetectionTargetMode>(clicked);
    changed = true;
  }
}

void DrawMaskGroup(SkinSettings& detection, bool& mask_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Mask", mask_open, changed)) return;

  changed |= DrawFloatRow(0, "structure_detect_edit_strength", "Edit Strength",
                          nullptr, &detection.edit_strength, nullptr, 1.0f, 0.1f,
                          8.0f, 1.0f);
  changed |= DrawFloatRow(0, "structure_detect_mask_smoothing", "Mask Smoothing",
                          nullptr, &detection.mask_smoothing, nullptr, 0.5f, 0.0f,
                          1.0f, 0.5f);
  changed |= DrawFloatRow(0, "structure_detect_mask_response", "Mask Response",
                          nullptr, &detection.mask_response, nullptr, 0.15f, 0.0f,
                          1.0f, 0.15f);
  changed |= DrawFloatRow(0, "structure_detect_luma_threshold", "Luma Threshold",
                          nullptr, &detection.luma_threshold, nullptr, 0.02f,
                          0.0f, 0.5f, 0.02f);
  changed |= DrawFloatRow(0, "structure_detect_luma_relative", "Luma Relative",
                          nullptr, &detection.luma_relative, nullptr, 0.05f,
                          0.0f, 1.0f, 0.05f);
  changed |= DrawFloatRow(0, "structure_detect_chroma_threshold",
                          "Chroma Threshold", nullptr,
                          &detection.chroma_threshold, nullptr, 0.02f, 0.0f, 0.5f,
                          0.02f);
  changed |= DrawFloatRow(0, "structure_detect_clip_threshold", "Clip Threshold",
                          nullptr, &detection.clip_threshold, nullptr, 0.97f, 0.5f,
                          1.0f, 0.97f);
}


}  // namespace

void DrawStructureDetectSection(Settings& settings, bool hdr_relevant, bool& changed) {
  if (!PersistentHeader(
          "Structure Detection", settings.ui_skin_detect_open, changed,
          "Run a secondary model evaluation before the first Pre-DLSS or Post-DLSS pass to build a current-frame structure mask.")) {
    return;
  }

  IndentScope section_indent;
  SkinSettings& detection = settings.skin;

  if (!hdr_relevant) ImGui::BeginDisabled();
  changed |= ImGui::Checkbox("Enable Structure Detection", &detection.enable);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip(
        "Run the neural model at reduced resolution before the first Pre/Post-DLSS pass and build an analysis mask from its edits."
        " The target can be skin-oriented or general structure.");
  }

  if (!hdr_relevant) ImGui::EndDisabled();
  if (!hdr_relevant) {
    ImGui::TextDisabled("Requires an enabled Pre-DLSS or Post-DLSS pass.");
    return;
  }

  WithDisabled(!detection.enable, [&] {
    DrawTargetGroup(detection, changed);
    float detection_scale = GetDetectionScale(detection);
    if (DrawFloatRow(0, "structure_detect_scale", "Detection Scale",
                     nullptr, &detection_scale, nullptr, 0.25f, 0.10f, 1.0f,
                     0.25f)) {
      SetDetectionScale(detection, detection_scale);
      changed = true;
    }
    DrawModelGroup(detection, settings.ui_skin_detect_model_open, changed);
    DrawMaskGroup(detection, settings.ui_skin_detect_mask_open, changed);
    ImGui::TextDisabled("Structure Detection uses the Game, Adaptive, and Local Exposure layers and their filter settings from the HDR section.");
  });
}

}  // namespace reshade_dlssnr

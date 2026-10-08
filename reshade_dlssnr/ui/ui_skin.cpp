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
  ImGui::SameLine(kControlLabelWidth);
  const int clicked = DrawGroupButtons("structure_detect_preset", kNames,
                                       kTips, 3, detection.preset, nullptr);
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
                                 &detection.ui_correction);
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
}

void DrawTargetGroup(SkinSettings& detection, bool& changed) {
  static const char* kNames[] = {"Skin", "Structure"};
  static const char* kTips[] = {
      "Use the model edit with the skin-color gate.",
      "Use the model edit without the skin-color gate. This can capture faces, clothing, geometry, and other structure.",
  };
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Detection Target");
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

void DrawExposureGroup(Settings& settings, SkinSettings& detection,
                       bool& hdr_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Detection Exposure", hdr_open, changed)) return;

  static const char* kNames[] = {"Follow Main", "Median", "Auto + Local"};
  static const char* kTips[] = {
      "Use the main HDR exposure and local-exposure settings.",
      "Use a dedicated median exposure for the analysis pass, with local exposure disabled.",
      "Use a dedicated histogram-driven eye adaptation and local exposure for the analysis pass.",
  };
  const int current = static_cast<int>(detection.exposure_mode);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Exposure Method");
  ImGui::SameLine(kControlLabelWidth);
  const int clicked = DrawGroupButtons("structure_detect_exposure_method",
                                       kNames, kTips, 3, current, nullptr);
  if (clicked >= 0 && clicked != current) {
    detection.exposure_mode = static_cast<DetectionExposureMode>(clicked);
    changed = true;
  }

  if (detection.exposure_mode == DetectionExposureMode::FollowMain) return;

  changed |= DrawFloatRow(0, "structure_detect_exposure_midtone", "Midtone Target",
                          nullptr, &detection.exposure_midtone, nullptr, 0.18f,
                          0.01f, 0.50f, 0.18f);
  changed |= DrawIntRow("structure_detect_exposure_interval", "Measure Interval",
                        nullptr, &detection.exposure_measure_interval, 32, 1, 4);

  if (detection.exposure_mode == DetectionExposureMode::Median) {
    changed |= DrawFloatRow(0, "structure_detect_exposure_rate_cap", "Response Cap",
                            nullptr, &detection.exposure_rate_cap, nullptr, 0.1f,
                            0.0f, 2.0f, 0.1f);
    return;
  }

  changed |= DrawFloatRow(0, "structure_detect_auto_low_percent", "Low Percent",
                          nullptr, &detection.exposure_auto_low_percent, nullptr,
                          0.70f, 0.0f, 0.99f, 0.70f);
  changed |= DrawFloatRow(0, "structure_detect_auto_high_percent", "High Percent",
                          nullptr, &detection.exposure_auto_high_percent, nullptr,
                          0.90f, 0.01f, 0.999f, 0.90f);
  changed |= DrawFloatRow(0, "structure_detect_speed_up", "Speed Up",
                          nullptr, &detection.exposure_speed_up, nullptr, 3.0f,
                          0.0f, 20.0f, 3.0f);
  changed |= DrawFloatRow(0, "structure_detect_speed_down", "Speed Down",
                          nullptr, &detection.exposure_speed_down, nullptr, 1.0f,
                          0.0f, 20.0f, 1.0f);
  changed |= DrawFloatRow(0, "structure_detect_local_exposure", "Local Exposure",
                          nullptr, &detection.exposure_local_strength, nullptr, 0.50f,
                          0.0f, 1.0f, 0.50f);
  changed |= DrawFloatRow(0, "structure_detect_local_brighten", "Max Brighten",
                          nullptr, &detection.exposure_local_max_brighten, nullptr,
                          1.75f, 0.0f, 4.0f, 1.75f);
  changed |= DrawFloatRow(0, "structure_detect_local_darken", "Max Darken",
                          nullptr, &detection.exposure_local_max_darken, nullptr,
                          1.75f, 0.0f, 4.0f, 1.75f);
}

}  // namespace

void DrawStructureDetectSection(Settings& settings, bool& changed) {
  if (!PersistentHeader("Structure Detection", settings.ui_skin_detect_open,
                        changed)) {
    return;
  }

  SkinSettings& detection = settings.skin;

  changed |= ImGui::Checkbox("Enable Structure Detection", &detection.enable);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Run the neural model at reduced resolution and build a persistent analysis mask from its edits."
        " The target can be skin-oriented or general structure.");
  }

  WithDisabled(!detection.enable, [&] {
    DrawTargetGroup(detection, changed);
    changed |= DrawFloatRow(0, "structure_detect_scale", "Detection Scale",
                            nullptr, &detection.scale, nullptr, 0.25f, 0.10f, 1.0f,
                            0.25f);
    DrawModelGroup(detection, settings.ui_skin_detect_model_open, changed);
    DrawMaskGroup(detection, settings.ui_skin_detect_mask_open, changed);
    DrawExposureGroup(settings, detection, settings.ui_skin_detect_hdr_open,
                      changed);
  });
}

}  // namespace reshade_dlssnr

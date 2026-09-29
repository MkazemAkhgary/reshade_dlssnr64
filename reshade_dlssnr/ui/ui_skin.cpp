#include <imgui.h>
#include <reshade.hpp>

#include "settings.h"
#include "settings/skin.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

namespace {

void DrawModelGroup(SkinSettings& skin, bool& model_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Model", model_open, changed)) return;

  static const char* kNames[] = {"A", "B", "C"};
  static const char* kTips[] = {"Preset A.", "Preset B.", "Preset C."};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Preset");
  ImGui::SameLine(kControlLabelWidth);
  const int clicked = DrawGroupButtons("skin_detect_preset", kNames, kTips, 3,
                                       skin.preset, nullptr);
  if (clicked >= 0) {
    skin.preset = clicked;
    changed = true;
  }

  changed |= DrawFloatRow(0, "skin_detect_intensity", "Intensity", nullptr,
                          &skin.intensity, nullptr, 1.0f, 0.0f, 2.0f, 1.0f);
  changed |= DrawFloatRow(0, "skin_detect_local_tone", "Local Tone", nullptr,
                          &skin.local_tone, nullptr, 0.0f, 0.0f, 2.0f, 0.0f);
  changed |= DrawFloatRow(0, "skin_detect_local_structure", "Local Structure",
                          nullptr, &skin.local_structure, nullptr, 0.0f,
                          0.0f, 2.0f, 0.0f);
  changed |= DrawFloatRow(0, "skin_detect_skin_structure", "Skin Structure",
                          nullptr, &skin.skin_structure, nullptr, 1.0f,
                          0.0f, 1.0f, 1.0f);

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("skin_detect_ui_correction",
                                 &skin.ui_correction);
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
}

void DrawMaskGroup(SkinSettings& skin, bool& mask_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Mask", mask_open, changed)) return;

  changed |= DrawFloatRow(0, "skin_detect_edit_strength", "Edit Strength",
                          nullptr, &skin.edit_strength, nullptr, 1.0f, 0.1f,
                          8.0f, 1.0f);
  changed |= DrawFloatRow(0, "skin_detect_mask_smoothing", "Mask Smoothing",
                          nullptr, &skin.mask_smoothing, nullptr, 0.5f, 0.0f,
                          1.0f, 0.5f);
  changed |= DrawFloatRow(0, "skin_detect_mask_response", "Mask Response",
                          nullptr, &skin.mask_response, nullptr, 0.15f, 0.0f,
                          1.0f, 0.15f);
}

void DrawHdrGroup(SkinSettings& skin, bool& hdr_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("HDR Response", hdr_open, changed)) return;

  changed |= DrawFloatRow(0, "skin_detect_hdr_midtone", "Midtone Target",
                          nullptr, &skin.hdr_midtone, nullptr, 0.18f, 0.01f,
                          0.50f, 0.18f);
  changed |= DrawFloatRow(0, "skin_detect_hdr_rate_cap", "Response Cap",
                          nullptr, &skin.hdr_rate_cap, nullptr, 0.1f, 0.0f,
                          2.0f, 0.1f);
  changed |= DrawIntRow("skin_detect_hdr_measure_interval", "Measure Interval",
                        nullptr, &skin.hdr_measure_interval, 32, 1, 16);
  changed |= DrawFloatRow(0, "skin_detect_hdr_depth_fallback_fraction",
                          "Depth Fallback", nullptr,
                          &skin.hdr_depth_fallback_fraction, nullptr, 0.2f,
                          0.0f, 1.0f, 1.0f);
}

}  // namespace

void DrawSkinDetectSection(Settings& settings, bool& changed) {
  if (!PersistentHeader("Skin Detection", settings.ui_skin_detect_open,
                        changed)) {
    return;
  }

  SkinSettings& skin = settings.skin;

  changed |= ImGui::Checkbox("Enable Skin Detection", &skin.enable);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Run the model at reduced resolution with tone and structure edits "
        "suppressed and skin edits on, and threshold the difference against "
        "its input to build a per-pixel skin mask.\n"
        "DLSS Output only.");
  }

  WithDisabled(!skin.enable, [&] {
    changed |= DrawFloatRow(0, "skin_detect_scale", "Detection Scale",
                            nullptr, &skin.scale, nullptr, 0.0f, 0.10f, 1.0f,
                            0.25f);
    changed |= DrawFloatRow(0, "skin_detect_luma_threshold", "Luma Threshold",
                            nullptr, &skin.luma_threshold, nullptr, 0.0f,
                            0.0f, 0.5f, 0.02f);
    changed |= DrawFloatRow(0, "skin_detect_luma_relative", "Luma Relative",
                            nullptr, &skin.luma_relative, nullptr, 0.0f, 0.0f,
                            1.0f, 0.05f);
    changed |= DrawFloatRow(0, "skin_detect_chroma_threshold",
                            "Chroma Threshold", nullptr,
                            &skin.chroma_threshold, nullptr, 0.0f, 0.0f, 0.5f,
                            0.02f);
    changed |= DrawFloatRow(0, "skin_detect_clip_threshold",
                            "Clip Threshold", nullptr, &skin.clip_threshold,
                            nullptr, 0.97f, 0.5f, 1.0f, 0.97f);

    DrawModelGroup(skin, settings.ui_skin_detect_model_open, changed);
    DrawMaskGroup(skin, settings.ui_skin_detect_mask_open, changed);
    DrawHdrGroup(skin, settings.ui_skin_detect_hdr_open, changed);
  });
}

}  // namespace reshade_dlssnr

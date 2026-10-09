// Structure/skin detection settings.
#pragma once

namespace reshade_dlssnr {

enum class DetectionTargetMode : int {
  Skin = 0,
  Structure = 1,
};

struct SkinSettings {
  bool enable = false;
  float scale = 0.25f;
  DetectionTargetMode target_mode = DetectionTargetMode::Skin;

  float luma_threshold = 0.02f;
  float luma_relative = 0.05f;
  float chroma_threshold = 0.02f;
  float clip_threshold = 0.97f;

  int preset = 0;
  float intensity = 1.0f;
  float local_tone = 0.0f;
  float local_structure = 0.0f;
  float skin_structure = 1.0f;
  bool ui_correction = false;

  float edit_strength = 1.0f;
  float mask_smoothing = 0.5f;
  float mask_response = 0.15f;
};

void LoadSkinSettings(SkinSettings& skin);
void SaveSkinSettings(const SkinSettings& skin);

}  // namespace reshade_dlssnr

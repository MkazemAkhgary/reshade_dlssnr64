// Model-guided structure detection settings: classifier thresholds, model
// tuning, mask smoothing, and the detection pass's own HDR exposure.
#pragma once

namespace reshade_dlssnr {

enum class DetectionTargetMode : int {
  Skin = 0,
  Structure = 1,
};

enum class DetectionExposureMode : int {
  FollowMain = 0,
  Median = 1,
  AutoLocal = 2,
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

  DetectionExposureMode exposure_mode = DetectionExposureMode::AutoLocal;
  float exposure_midtone = 0.18f;
  float exposure_rate_cap = 0.1f;
  int exposure_measure_interval = 4;
  float exposure_auto_low_percent = 0.70f;
  float exposure_auto_high_percent = 0.90f;
  float exposure_speed_up = 3.0f;
  float exposure_speed_down = 1.0f;
  float exposure_local_strength = 0.50f;
  float exposure_local_max_brighten = 1.75f;
  float exposure_local_max_darken = 1.75f;
};

void LoadSkinSettings(SkinSettings& skin);
void SaveSkinSettings(const SkinSettings& skin);

}  // namespace reshade_dlssnr

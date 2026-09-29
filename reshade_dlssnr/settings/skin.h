// Skin detection settings: classifier thresholds, model tuning, mask
// smoothing, and the detection pass's own HDR response.
#pragma once

namespace reshade_dlssnr {

struct SkinSettings {
  bool enable = false;
  float scale = 0.25f;

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

  float hdr_midtone = 0.18f;
  float hdr_rate_cap = 0.1f;
  int hdr_measure_interval = 16;
  float hdr_depth_fallback_fraction = 1.0f;
};

}  // namespace reshade_dlssnr

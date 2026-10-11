#pragma once

namespace reshade_dlssnr {

struct ImportanceMaskSettings {
  bool enable = false;
  float luma_threshold = 0.02f;
  float luma_relative = 0.05f;
  float chroma_threshold = 0.02f;
  float edit_strength = 1.0f;
  float mask_smoothing = 0.5f;
  float mask_response = 0.15f;
};

void LoadImportanceMaskSettings(ImportanceMaskSettings& settings);
void SaveImportanceMaskSettings(const ImportanceMaskSettings& settings);

}  // namespace reshade_dlssnr

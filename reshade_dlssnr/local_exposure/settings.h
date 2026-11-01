// Local exposure: a spatial operator applied after global exposure.
//
// It is the one exposure layer that needs no HDR input. Its pivot is a
// display-referred luminance, so a scene-referred source reaches it through the
// tone curve and a display-referred source reaches it directly.
#pragma once

namespace reshade_dlssnr {

struct LocalExposureSettings {
  bool enabled = false;
  float strength = 0.5f;
  float max_brighten = 2.50f;
  float max_darken = 2.50f;
  float midtone = 0.18f;
  float shadow_strength = 1.0f;
  float highlight_strength = 1.0f;
  float shadow_range = 1.0f;
  float highlight_range = 1.0f;
  float highlight_protection = 1.0f;
  float shadow_protection = 1.0f;
  float detail_retention = 1.0f;
  float edge_protection = 0.5f;
  float radius_px = 8.0f;
  int samples = 8;
  bool restore_exposure = false;
  // 0 = exact per-pixel restoration; 1 = uniform global restoration.
  // Intermediate values interpolate the correction in log exposure space.
  float restore_mode = 0.5f;
  float restore_strength = 1.0f;
};

}  // namespace reshade_dlssnr

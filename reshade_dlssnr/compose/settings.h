// Residual edit settings: how the model's edit is carried onto the source.
#pragma once

namespace reshade_dlssnr {

struct ResidualSettings {
  float edit_strength = 1.0f;
  float chroma_strength = 1.0f;
  float edit_gamma = 1.0f;
  float highlight_protect = 0.0f;
  float shadow_protect = 0.0f;

  // Edge-preservation exponent for the joint-bilateral upsample of the
  // model's input and output. Higher values follow the full-resolution
  // image's edges more sharply; lower values blend more across the
  // low-resolution neighborhood. It does not scale the residual itself.
  float upsample_sharpness = 8.0f;
  float ratio_eps = 1e-3f;
  float ratio_brighten = 4.0f;
  float ratio_darken = 1.0f / 16.0f;
  bool ratio_clamps_enabled = true;

};

// Strength section: blends of the model's luma and chroma edits applied
// when the model output is composed back onto the source.
struct StrengthSettings {
  float luma_strength = 1.0f;
  float chroma_strength = 1.0f;
};

}  // namespace reshade_dlssnr

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
  float ratio_eps = 1e-2f;
  float ratio_brighten = 64.0f;
  float ratio_darken = 1.0f / 16.0f;

};

// Strength section: blends of the model's luma and chroma edits applied
// when the model output is composed back onto the source.
struct StrengthSettings {
  float luma_strength = 1.0f;
  float chroma_strength = 1.0f;
};

// Soft compression of the residual delta before it is blended onto the
// source. Sliders are gated on `enabled`.
struct SoftCompressionSettings {
  bool enabled = false;
  float max_brighten = 0.5f;
  float max_darken = 0.5f;
  float max_chroma = 0.5f;
  float knee = 1.0f;
};

}  // namespace reshade_dlssnr

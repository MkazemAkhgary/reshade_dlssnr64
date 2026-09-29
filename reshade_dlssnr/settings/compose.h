// Residual edit settings: how the model's edit is carried onto the source.
#pragma once

namespace reshade_dlssnr {

struct ResidualSettings {
  float edit_strength = 1.0f;
  float chroma_strength = 1.0f;
  float edit_gamma = 1.0f;
  float highlight_protect = 0.0f;
  float shadow_protect = 0.0f;

  float ratio_eps = 1e-2f;
  float ratio_brighten = 64.0f;
  float ratio_darken = 1.0f / 16.0f;
};

}  // namespace reshade_dlssnr

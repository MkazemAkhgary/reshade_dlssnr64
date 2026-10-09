// Residual edit settings: how the model's edit is carried onto the source.
#pragma once

#include <string>

namespace reshade_dlssnr {

enum class HdrComposeMode : int {
  Ratio = 0,
  InverseToneMap = 1,
};

struct ResidualSettings {
  float edit_strength = 1.0f;
  float chroma_strength = 1.0f;
  float edit_gamma = 1.0f;
  float highlight_protect = 0.0f;
  float shadow_protect = 0.0f;

  float ratio_eps = 1e-2f;
  float ratio_brighten = 64.0f;
  float ratio_darken = 1.0f / 16.0f;

  // HDR-only compose method. Both modes transfer the model edit as a relative
  // change without inverting the nonlinear super-white fit at composition time.
  // InverseToneMap (persisted value retained for compatibility) applies that
  // relative edit through GainMapSplit's exact per-pixel reciprocal multiplier.
  HdrComposeMode hdr_compose_mode = HdrComposeMode::Ratio;
};

void LoadResidual(const std::string& prefix, ResidualSettings& residual);
void SaveResidual(const std::string& prefix, const ResidualSettings& residual);

}  // namespace reshade_dlssnr

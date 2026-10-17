// HDR settings and independent, stackable exposure layers.
#pragma once

#include <algorithm>
#include <string>

#include "shaders/common/color_constants.h"

namespace reshade_dlssnr {

enum class HdrFormat : int {
  Auto = 0,
  Sdr = 1,
  ScRgb = 2,
  Hdr10 = 3,
};

struct ExposureFilterSettings {
  bool importance_mask = false;
  bool depth = false;
  bool percentile_range = false;
  float depth_near = 0.0f;
  float depth_far = 1.0f;
  float percentile_low = 0.70f;
  float percentile_high = 0.90f;
};

struct GameExposureLayerSettings {
  bool enabled = true;
  float bias_stops = 0.0f;
};

struct AdaptiveExposureLayerSettings {
  bool enabled = true;
  float midtone = 0.18f;
  float speed_up = 3.0f;
  float speed_down = 1.0f;
  // When Game Exposure is active, this is a 0..1 blend toward the adaptive
  // target, still limited by max_correction_stops.
  float strength_over_game = 0.0f;
  float max_correction_stops = 0.75f;
  ExposureFilterSettings filters = {false, false, true};
};

struct LocalExposureLayerSettings {
  bool enabled = false;
  float strength = 0.50f;
  float max_brighten = 1.75f;
  float max_darken = 1.75f;
  float midtone = 0.18f;
  // 0 = no restore, 1 = full restore. The grade pass divides the model
  // output by lerp(1, mean local scale, strength) in linear light, so an
  // intermediate value blends partway back toward the global exposure.
  float restore_global_exposure = 0.0f;
  bool precomputed_luma = false;
  bool fp16_reference = false;
};

// Encoding code consumed by the shaders: 0 sRGB, 1 scRGB, 2 HDR10.
inline int HdrFormatCode(HdrFormat format) {
  switch (format) {
    case HdrFormat::ScRgb: return 1;
    case HdrFormat::Hdr10: return 2;
    case HdrFormat::Auto:
    case HdrFormat::Sdr:
    default: return 0;
  }
}

struct HdrSettings {
  bool enable = true;
  HdrFormat format = HdrFormat::Auto;
  int measure_interval = 4;
  float highlight_headroom = 1.0f;
  bool detailed_statistics = false;
  bool faster_metering = false;

  GameExposureLayerSettings game_exposure = {};
  AdaptiveExposureLayerSettings adaptive_exposure = {};
  LocalExposureLayerSettings local_exposure = {};

  // These change the transfer curve, not the interpretation of exposure.
  float bt2446_k1 = 0.83802f;
  float bt2446_k2 = 15.09968f;
  float bt2446_k3 = 0.74204f;
  float bt2446_k4 = 78.99439f;
  float bt2446_crosstalk = 0.04f;

  float gain_min_log2 = -4.0f;
  float gain_max_log2 = 6.0f;
};

// Persistence is not here. The values are declared in
// feature/hdr/schema.cpp, read by feature/hdr/view.cpp, and round-tripped by
// ctl/serialize -- so a new setting needs no loader and no saver of its own.

inline float CalcEffectiveSceneWhite(float scene_white, const HdrSettings& hdr) {
  return scene_white * std::max(hdr.highlight_headroom, 0.01f);
}

inline float CalcEffectiveSceneWhiteNits(float scene_white, const HdrSettings& hdr) {
  return CalcEffectiveSceneWhite(scene_white, hdr) *
         color_constants::kSdrReferenceWhiteNits;
}

}  // namespace reshade_dlssnr

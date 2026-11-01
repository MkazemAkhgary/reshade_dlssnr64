// HDR settings and the two global exposure layers that feed them.
//
// Local exposure is not here: it needs no HDR input, so it is a pass of its own
// (local_exposure/settings.h) rather than a third layer of this stack.
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
  bool exposure_mask = false;
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
  bool enabled = false;
  float midtone = 0.18f;
  float speed_up = 3.0f;
  float speed_down = 1.0f;
  // When Game Exposure is active, this is a 0..1 blend toward the adaptive
  // target, still limited by max_correction_stops.
  float strength_over_game = 0.0f;
  float max_correction_stops = 0.75f;
  ExposureFilterSettings filters = {};
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
  // 0 = every pixel, 1 = every 2nd in each axis, 2 = every 4th, 3 = every 8th.
  int metering_density = 0;
  bool reuse_previous_pass_measurement = false;

  GameExposureLayerSettings game_exposure = {};
  AdaptiveExposureLayerSettings adaptive_exposure = {};

  // BT.2446C tone-curve source parameters; derived k2/k4 are computed on CPU.
  float bt2446_k1 = 0.83802f;
  float bt2446_k3 = 0.74204f;
  float bt2446_ip = 94.26f;
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

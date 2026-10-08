// HDR settings: format detection, histogram exposure, and local exposure.
#pragma once

#include <algorithm>

#include "shaders/common/color_constants.h"

namespace reshade_dlssnr {

enum class HdrFormat : int {
  Auto  = 0,
  Sdr   = 1,
  ScRgb = 2,
  Hdr10 = 3,
};

enum class HdrExposureMethod : int {
  Median = 0,
  AutoLocal = 1,
  GameExposureBuffer = 2,  // Read the game's NGX ExposureTexture.
};

// Encoding code the shaders read: 0 sRGB, 1 scRGB, 2 HDR10.
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
  HdrExposureMethod exposure_method = HdrExposureMethod::AutoLocal;
  float midtone = 0.18f;
  float white_percentile = 0.99f;
  float highlight_headroom = 8.0f;

  // AutoLocal follows the same high-level split used by UE5: global eye
  // adaptation drives a stable target, then a local luminance stage preserves
  // shadow detail without forcing the whole frame to match that target.
  float auto_low_percent = 0.70f;
  float auto_high_percent = 0.90f;
  float speed_up = 3.0f;
  float speed_down = 1.0f;
  float local_exposure_strength = 0.50f;
  float local_max_brighten = 1.75f;
  float local_max_darken = 1.75f;

  // Median-method response cap in log2 stops per measurement.
  float rate_cap = 0.1f;

  // BT.2446-C tuning. These defaults match the reference implementation
  // used for the gain-map architecture and are intentionally persisted so
  // advanced users can tune the curve without changing the shader.
  float bt2446_k1 = 0.83802f;
  float bt2446_k2 = 15.09968f;
  float bt2446_k3 = 0.74204f;
  float bt2446_k4 = 78.99439f;
  float bt2446_crosstalk = 0.04f;

  // Raw gain-map metadata. A is a single luma gain in linear space; the
  // offsets are applied to luminance during gain generation/application.
  // They do not get added independently to RGB channels.
  float gain_base_offset = 1.0f / 64.0f;
  float gain_alternate_offset = 1.0f / 64.0f;
  float gain_min_log2 = -4.0f;
  float gain_max_log2 = 6.0f;

  // Structure-mask exposure weighting. The mask becomes an additional normalized
  // metering population for later frames; these values control when it is
  // eligible and how much of the effective histogram it contributes.
  float skin_fraction_threshold = 0.01f;
  float skin_priority = 0.7f;
  float depth_fallback_fraction = 0.20f; // Reserved for depth-gated metering.
};

void LoadHdrSettings(HdrSettings& hdr);
void SaveHdrSettings(const HdrSettings& hdr);

inline float CalcEffectiveSceneWhite(float scene_white, const HdrSettings& hdr)
{
    return scene_white * std::max(hdr.highlight_headroom, 0.01f);
}

inline float CalcEffectiveSceneWhiteNits(float scene_white, const HdrSettings& hdr)
{
    return CalcEffectiveSceneWhite(scene_white, hdr) *
           color_constants::kSdrReferenceWhiteNits;
}

}  // namespace reshade_dlssnr

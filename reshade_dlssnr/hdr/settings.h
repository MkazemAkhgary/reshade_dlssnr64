// HDR settings: format detection, exposure metering, and skin priority.
#pragma once

#include <algorithm>

namespace reshade_dlssnr {

enum class HdrFormat : int {
  Auto  = 0,
  Sdr   = 1,
  ScRgb = 2,
  Hdr10 = 3,
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
  float midtone = 0.18f;
  float white_percentile = 0.99f;
  float highlight_headroom = 8.0f;
  float rate_cap = 0.1f;

  // BT.2446-C tuning. These defaults match the reference implementation
  // used for the gain-map architecture and are intentionally persisted so
  // advanced users can tune the curve without changing the shader.
  float bt2446_k1 = 0.83802f;
  float bt2446_k2 = 15.09968f;
  float bt2446_k3 = 0.74204f;
  float bt2446_k4 = 78.99439f;
  float bt2446_crosstalk = 0.04f;

  // Raw gain-map metadata. The gain channel itself remains raw f32 log2;
  // these bounds are metadata/diagnostic limits rather than quantization.
  float gain_base_offset = 1.0f / 64.0f;
  float gain_alternate_offset = 1.0f / 64.0f;
  float gain_min_log2 = -4.0f;
  float gain_max_log2 = 6.0f;

  float skin_fraction_threshold = 0.01f;
  float skin_priority = 0.7f;
  float depth_fallback_fraction = 0.20f;
};

void LoadHdrSettings(HdrSettings& hdr);
void SaveHdrSettings(const HdrSettings& hdr);

inline float CalcEffectiveSceneWhite(float scene_white, const HdrSettings& hdr)
{
    return scene_white * std::max(hdr.highlight_headroom, 0.01f);
}

}  // namespace reshade_dlssnr

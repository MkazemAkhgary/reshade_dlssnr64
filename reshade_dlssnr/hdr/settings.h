// HDR settings: format detection, exposure metering, and skin priority.
#pragma once

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

  float skin_fraction_threshold = 0.01f;
  float skin_priority = 0.7f;
  float depth_fallback_fraction = 0.20f;
};

void LoadHdrSettings(HdrSettings& hdr);
void SaveHdrSettings(const HdrSettings& hdr);

}  // namespace reshade_dlssnr

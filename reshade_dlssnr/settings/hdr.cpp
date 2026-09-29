#include "settings/hdr.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadHdrSettings(HdrSettings& hdr) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            hdr.enable);

  int format_value = static_cast<int>(hdr.format);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            format_value);
  if (format_value < 0 || format_value > 3) format_value = 0;
  hdr.format = static_cast<HdrFormat>(format_value);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.MeasureInterval",
                            hdr.measure_interval);
  if (hdr.measure_interval < 1) hdr.measure_interval = 1;
  if (hdr.measure_interval > 32) hdr.measure_interval = 32;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            hdr.midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            hdr.white_percentile);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            hdr.highlight_headroom);
  if (hdr.highlight_headroom > 16.0f || hdr.highlight_headroom < 0.25f) {
    hdr.highlight_headroom = 8.0f;
  }

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.RateCap",
                            hdr.rate_cap);
  if (hdr.rate_cap < 0.0f) hdr.rate_cap = 0.0f;
  if (hdr.rate_cap > 2.0f) hdr.rate_cap = 2.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SkinFractionThreshold",
                            hdr.skin_fraction_threshold);
  if (hdr.skin_fraction_threshold < 0.0f) hdr.skin_fraction_threshold = 0.0f;
  if (hdr.skin_fraction_threshold > 0.5f) hdr.skin_fraction_threshold = 0.5f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.SkinPriority",
                            hdr.skin_priority);
  if (hdr.skin_priority < 0.0f) hdr.skin_priority = 0.0f;
  if (hdr.skin_priority > 1.0f) hdr.skin_priority = 1.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.DepthFallbackFraction",
                            hdr.depth_fallback_fraction);
  if (hdr.depth_fallback_fraction < 0.0f) hdr.depth_fallback_fraction = 0.0f;
  if (hdr.depth_fallback_fraction > 1.0f) hdr.depth_fallback_fraction = 1.0f;
}

void SaveHdrSettings(const HdrSettings& hdr) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            hdr.enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            static_cast<int>(hdr.format));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.MeasureInterval",
                            hdr.measure_interval);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            hdr.midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            hdr.white_percentile);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            hdr.highlight_headroom);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.RateCap",
                            hdr.rate_cap);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SkinFractionThreshold",
                            hdr.skin_fraction_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.SkinPriority",
                            hdr.skin_priority);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.DepthFallbackFraction",
                            hdr.depth_fallback_fraction);
}

}  // namespace reshade_dlssnr

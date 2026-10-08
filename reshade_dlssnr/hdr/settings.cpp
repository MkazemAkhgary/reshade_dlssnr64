#include "hdr/settings.h"

#include <algorithm>

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

  int exposure_method = static_cast<int>(hdr.exposure_method);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.ExposureMethod", exposure_method);
  if (exposure_method < 0 || exposure_method > 1) exposure_method = 1;
  hdr.exposure_method = static_cast<HdrExposureMethod>(exposure_method);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            hdr.midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            hdr.white_percentile);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            hdr.highlight_headroom);
  if (hdr.highlight_headroom > 16.0f || hdr.highlight_headroom < 0.25f) {
    hdr.highlight_headroom = 8.0f;
  }

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.AutoLowPercent", hdr.auto_low_percent);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.AutoHighPercent", hdr.auto_high_percent);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SpeedUp", hdr.speed_up);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SpeedDown", hdr.speed_down);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalExposureStrength",
                            hdr.local_exposure_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalMaxBrighten", hdr.local_max_brighten);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalMaxDarken", hdr.local_max_darken);
  hdr.auto_low_percent = std::clamp(hdr.auto_low_percent, 0.0f, 0.99f);
  hdr.auto_high_percent = std::clamp(hdr.auto_high_percent, 0.01f, 0.999f);
  if (hdr.auto_high_percent <= hdr.auto_low_percent) {
    hdr.auto_low_percent = 0.70f;
    hdr.auto_high_percent = 0.90f;
  }
  hdr.speed_up = std::clamp(hdr.speed_up, 0.0f, 20.0f);
  hdr.speed_down = std::clamp(hdr.speed_down, 0.0f, 20.0f);
  hdr.local_exposure_strength = std::clamp(hdr.local_exposure_strength, 0.0f, 1.0f);
  hdr.local_max_brighten = std::clamp(hdr.local_max_brighten, 0.0f, 4.0f);
  hdr.local_max_darken = std::clamp(hdr.local_max_darken, 0.0f, 4.0f);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K1",
                            hdr.bt2446_k1);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K2",
                            hdr.bt2446_k2);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K3",
                            hdr.bt2446_k3);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K4",
                            hdr.bt2446_k4);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.Crosstalk",
                            hdr.bt2446_crosstalk);
  hdr.bt2446_k1 = std::max(hdr.bt2446_k1, 1e-4f);
  hdr.bt2446_k2 = std::max(hdr.bt2446_k2, 1e-4f);
  hdr.bt2446_k3 = std::clamp(hdr.bt2446_k3, 0.001f, 0.999f);
  hdr.bt2446_k4 = std::max(hdr.bt2446_k4, 1e-4f);
  hdr.bt2446_crosstalk = std::clamp(hdr.bt2446_crosstalk, 0.0f, 0.33f);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.BaseOffset",
                            hdr.gain_base_offset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.AlternateOffset",
                            hdr.gain_alternate_offset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.MinLog2", hdr.gain_min_log2);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.MaxLog2", hdr.gain_max_log2);
  hdr.gain_base_offset = std::clamp(hdr.gain_base_offset, 0.0f, 1.0f);
  hdr.gain_alternate_offset = std::clamp(hdr.gain_alternate_offset, 0.0f, 1.0f);
  hdr.gain_min_log2 = std::clamp(hdr.gain_min_log2, -32.0f, 0.0f);
  hdr.gain_max_log2 = std::clamp(hdr.gain_max_log2, 0.0f, 32.0f);
  if (hdr.gain_max_log2 <= hdr.gain_min_log2) {
    hdr.gain_min_log2 = -4.0f;
    hdr.gain_max_log2 = 6.0f;
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
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.ExposureMethod",
                            static_cast<int>(hdr.exposure_method));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            hdr.midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            hdr.white_percentile);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            hdr.highlight_headroom);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.AutoLowPercent", hdr.auto_low_percent);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.AutoHighPercent", hdr.auto_high_percent);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SpeedUp", hdr.speed_up);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.SpeedDown", hdr.speed_down);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalExposureStrength",
                            hdr.local_exposure_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalMaxBrighten", hdr.local_max_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.LocalMaxDarken", hdr.local_max_darken);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K1",
                            hdr.bt2446_k1);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K2",
                            hdr.bt2446_k2);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K3",
                            hdr.bt2446_k3);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.K4",
                            hdr.bt2446_k4);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.BT2446.Crosstalk",
                            hdr.bt2446_crosstalk);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.BaseOffset", hdr.gain_base_offset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.AlternateOffset", hdr.gain_alternate_offset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.MinLog2", hdr.gain_min_log2);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "Hdr.GainMap.MaxLog2", hdr.gain_max_log2);
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

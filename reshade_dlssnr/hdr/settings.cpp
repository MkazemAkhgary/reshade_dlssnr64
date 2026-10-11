#include "hdr/settings.h"

#include <algorithm>
#include <cstdio>

#include <reshade.hpp>

namespace reshade_dlssnr {
namespace {

void LoadBool(const char* key, bool& value) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}
void LoadInt(const char* key, int& value) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}
void LoadFloat(const char* key, float& value) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}
void SaveBool(const char* key, bool value) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}
void SaveInt(const char* key, int value) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}
void SaveFloat(const char* key, float value) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key, value);
}

void LoadFilter(const char* prefix, ExposureFilterSettings& f) {
  char key[128];
  auto boolean = [&](const char* suffix, bool& value) {
    std::snprintf(key, sizeof(key), "%s.%s", prefix, suffix);
    LoadBool(key, value);
  };
  auto number = [&](const char* suffix, float& value) {
    std::snprintf(key, sizeof(key), "%s.%s", prefix, suffix);
    LoadFloat(key, value);
  };
  boolean("StructureMask", f.structure_mask);
  boolean("Depth", f.depth);
  boolean("PercentileRange", f.percentile_range);
  number("DepthNear", f.depth_near);
  number("DepthFar", f.depth_far);
  number("PercentileLow", f.percentile_low);
  number("PercentileHigh", f.percentile_high);
  f.depth_near = std::clamp(f.depth_near, 0.0f, 1.0f);
  f.depth_far = std::clamp(f.depth_far, 0.0f, 1.0f);
  if (f.depth_near > f.depth_far) std::swap(f.depth_near, f.depth_far);
  f.percentile_low = std::clamp(f.percentile_low, 0.0f, 0.99f);
  f.percentile_high = std::clamp(f.percentile_high, 0.01f, 1.0f);
  if (f.percentile_high <= f.percentile_low) {
    f.percentile_low = 0.70f;
    f.percentile_high = 0.90f;
  }
}

void SaveFilter(const char* prefix, const ExposureFilterSettings& f) {
  char key[128];
  auto boolean = [&](const char* suffix, bool value) {
    std::snprintf(key, sizeof(key), "%s.%s", prefix, suffix);
    SaveBool(key, value);
  };
  auto number = [&](const char* suffix, float value) {
    std::snprintf(key, sizeof(key), "%s.%s", prefix, suffix);
    SaveFloat(key, value);
  };
  boolean("StructureMask", f.structure_mask);
  boolean("Depth", f.depth);
  boolean("PercentileRange", f.percentile_range);
  number("DepthNear", f.depth_near);
  number("DepthFar", f.depth_far);
  number("PercentileLow", f.percentile_low);
  number("PercentileHigh", f.percentile_high);
}

void LoadLegacyDefaults(HdrSettings& hdr) {
  int old_method = -1;
  LoadInt("Hdr.ExposureMethod", old_method);
  if (old_method < 0 || old_method > 2) return;

  // Convert the previous exclusive mode into its nearest layer-stack equivalent.
  hdr.game_exposure.enabled = old_method == 2;
  hdr.adaptive_exposure.enabled = old_method != 2;
  hdr.local_exposure.enabled = old_method == 1;
  LoadFloat("Hdr.Midtone", hdr.adaptive_exposure.midtone);
  LoadFloat("Hdr.AutoLowPercent", hdr.adaptive_exposure.filters.percentile_low);
  LoadFloat("Hdr.AutoHighPercent", hdr.adaptive_exposure.filters.percentile_high);
  LoadFloat("Hdr.SpeedUp", hdr.adaptive_exposure.speed_up);
  LoadFloat("Hdr.SpeedDown", hdr.adaptive_exposure.speed_down);
  LoadFloat("Hdr.LocalExposureStrength", hdr.local_exposure.strength);
  LoadFloat("Hdr.LocalMaxBrighten", hdr.local_exposure.max_brighten);
  LoadFloat("Hdr.LocalMaxDarken", hdr.local_exposure.max_darken);
  hdr.adaptive_exposure.filters.percentile_range = old_method == 1;
}

}  // namespace

void LoadHdrSettings(HdrSettings& hdr) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable", hdr.enable);
  int format = static_cast<int>(hdr.format);
  LoadInt("Hdr.Format", format);
  if (format < 0 || format > 3) format = 0;
  hdr.format = static_cast<HdrFormat>(format);
  LoadInt("Hdr.MeasureInterval", hdr.measure_interval);
  hdr.measure_interval = std::clamp(hdr.measure_interval, 1, 32);
  LoadFloat("Hdr.HighlightHeadroom", hdr.highlight_headroom);
  if (hdr.highlight_headroom < 0.25f || hdr.highlight_headroom > 16.0f)
    hdr.highlight_headroom = 8.0f;

  int schema_version = 0;
  LoadInt("Hdr.LayersVersion", schema_version);
  if (schema_version < 1) LoadLegacyDefaults(hdr);

  LoadBool("Hdr.Layer.Game.Enabled", hdr.game_exposure.enabled);
  LoadFloat("Hdr.Layer.Game.BiasStops", hdr.game_exposure.bias_stops);
  LoadBool("Hdr.Layer.Game.BoundedTuning", hdr.game_exposure.bounded_tuning);
  LoadFloat("Hdr.Layer.Game.TuningStrength", hdr.game_exposure.tuning_strength);
  LoadFloat("Hdr.Layer.Game.MaxTuningStops", hdr.game_exposure.max_tuning_stops);
  LoadFloat("Hdr.Layer.Game.TuningMidtone", hdr.game_exposure.tuning_midtone);
  LoadFloat("Hdr.Layer.Game.TuningSpeedUp", hdr.game_exposure.tuning_speed_up);
  LoadFloat("Hdr.Layer.Game.TuningSpeedDown", hdr.game_exposure.tuning_speed_down);
  LoadFilter("Hdr.Layer.Game.Filter", hdr.game_exposure.filters);

  LoadBool("Hdr.Layer.Adaptive.Enabled", hdr.adaptive_exposure.enabled);
  LoadFloat("Hdr.Layer.Adaptive.Midtone", hdr.adaptive_exposure.midtone);
  LoadFloat("Hdr.Layer.Adaptive.SpeedUp", hdr.adaptive_exposure.speed_up);
  LoadFloat("Hdr.Layer.Adaptive.SpeedDown", hdr.adaptive_exposure.speed_down);
  LoadFloat("Hdr.Layer.Adaptive.StrengthOverGame", hdr.adaptive_exposure.strength_over_game);
  LoadFloat("Hdr.Layer.Adaptive.MaxCorrectionStops", hdr.adaptive_exposure.max_correction_stops);
  LoadFilter("Hdr.Layer.Adaptive.Filter", hdr.adaptive_exposure.filters);

  LoadBool("Hdr.Layer.Local.Enabled", hdr.local_exposure.enabled);
  LoadFloat("Hdr.Layer.Local.Strength", hdr.local_exposure.strength);
  LoadFloat("Hdr.Layer.Local.MaxBrighten", hdr.local_exposure.max_brighten);
  LoadFloat("Hdr.Layer.Local.MaxDarken", hdr.local_exposure.max_darken);
  LoadFloat("Hdr.Layer.Local.Midtone", hdr.local_exposure.midtone);
  LoadFloat("Hdr.Layer.Local.BlockSizePx", hdr.local_exposure.block_size_px);
  LoadInt("Hdr.Layer.Local.SampleCount", hdr.local_exposure.sample_count);
  LoadFloat("Hdr.Layer.Local.EdgeSharpness", hdr.local_exposure.edge_sharpness);
  LoadBool("Hdr.Layer.Local.DepthContinuity", hdr.local_exposure.depth_continuity);
  LoadFloat("Hdr.Layer.Local.DepthEdgeThreshold", hdr.local_exposure.depth_edge_threshold);
  LoadFilter("Hdr.Layer.Local.Filter", hdr.local_exposure.filters);

  hdr.game_exposure.bias_stops = std::clamp(hdr.game_exposure.bias_stops, -4.0f, 4.0f);
  hdr.game_exposure.tuning_strength = std::clamp(hdr.game_exposure.tuning_strength, 0.0f, 1.0f);
  hdr.game_exposure.max_tuning_stops = std::clamp(hdr.game_exposure.max_tuning_stops, 0.0f, 2.0f);
  hdr.game_exposure.tuning_midtone = std::clamp(hdr.game_exposure.tuning_midtone, 0.01f, 0.5f);
  hdr.game_exposure.tuning_speed_up = std::clamp(hdr.game_exposure.tuning_speed_up, 0.0f, 20.0f);
  hdr.game_exposure.tuning_speed_down = std::clamp(hdr.game_exposure.tuning_speed_down, 0.0f, 20.0f);
  hdr.adaptive_exposure.midtone = std::clamp(hdr.adaptive_exposure.midtone, 0.01f, 0.5f);
  hdr.adaptive_exposure.speed_up = std::clamp(hdr.adaptive_exposure.speed_up, 0.0f, 20.0f);
  hdr.adaptive_exposure.speed_down = std::clamp(hdr.adaptive_exposure.speed_down, 0.0f, 20.0f);
  hdr.adaptive_exposure.strength_over_game = std::clamp(hdr.adaptive_exposure.strength_over_game, 0.0f, 1.0f);
  hdr.adaptive_exposure.max_correction_stops = std::clamp(hdr.adaptive_exposure.max_correction_stops, 0.0f, 2.0f);
  hdr.local_exposure.strength = std::clamp(hdr.local_exposure.strength, 0.0f, 1.0f);
  hdr.local_exposure.max_brighten = std::clamp(hdr.local_exposure.max_brighten, 0.0f, 4.0f);
  hdr.local_exposure.max_darken = std::clamp(hdr.local_exposure.max_darken, 0.0f, 4.0f);
  hdr.local_exposure.midtone = std::clamp(hdr.local_exposure.midtone, 0.01f, 0.5f);
  hdr.local_exposure.block_size_px = std::clamp(hdr.local_exposure.block_size_px, 4.0f, 512.0f);
  hdr.local_exposure.sample_count = std::clamp(hdr.local_exposure.sample_count, 4, 48);
  hdr.local_exposure.edge_sharpness = std::clamp(hdr.local_exposure.edge_sharpness, 0.0f, 8.0f);
  hdr.local_exposure.depth_edge_threshold = std::clamp(hdr.local_exposure.depth_edge_threshold, 0.001f, 0.1f);

  LoadFloat("Hdr.BT2446.K1", hdr.bt2446_k1);
  LoadFloat("Hdr.BT2446.K2", hdr.bt2446_k2);
  LoadFloat("Hdr.BT2446.K3", hdr.bt2446_k3);
  LoadFloat("Hdr.BT2446.K4", hdr.bt2446_k4);
  LoadFloat("Hdr.BT2446.Crosstalk", hdr.bt2446_crosstalk);
  hdr.bt2446_k1 = std::max(hdr.bt2446_k1, 1e-4f);
  hdr.bt2446_k2 = std::max(hdr.bt2446_k2, 1e-4f);
  hdr.bt2446_k3 = std::clamp(hdr.bt2446_k3, 0.001f, 0.999f);
  hdr.bt2446_k4 = std::max(hdr.bt2446_k4, 1e-4f);
  hdr.bt2446_crosstalk = std::clamp(hdr.bt2446_crosstalk, 0.0f, 0.33f);

  LoadFloat("Hdr.GainMap.MinLog2", hdr.gain_min_log2);
  LoadFloat("Hdr.GainMap.MaxLog2", hdr.gain_max_log2);
  hdr.gain_min_log2 = std::clamp(hdr.gain_min_log2, -32.0f, 0.0f);
  hdr.gain_max_log2 = std::clamp(hdr.gain_max_log2, 0.0f, 32.0f);
  if (hdr.gain_max_log2 <= hdr.gain_min_log2) {
    hdr.gain_min_log2 = -4.0f;
    hdr.gain_max_log2 = 6.0f;
  }
}

void SaveHdrSettings(const HdrSettings& hdr) {
  SaveBool("Hdr.Enable", hdr.enable);
  SaveInt("Hdr.Format", static_cast<int>(hdr.format));
  SaveInt("Hdr.MeasureInterval", hdr.measure_interval);
  SaveInt("Hdr.LayersVersion", 2);

  SaveBool("Hdr.Layer.Game.Enabled", hdr.game_exposure.enabled);
  SaveFloat("Hdr.Layer.Game.BiasStops", hdr.game_exposure.bias_stops);
  SaveBool("Hdr.Layer.Game.BoundedTuning", hdr.game_exposure.bounded_tuning);
  SaveFloat("Hdr.Layer.Game.TuningStrength", hdr.game_exposure.tuning_strength);
  SaveFloat("Hdr.Layer.Game.MaxTuningStops", hdr.game_exposure.max_tuning_stops);
  SaveFloat("Hdr.Layer.Game.TuningMidtone", hdr.game_exposure.tuning_midtone);
  SaveFloat("Hdr.Layer.Game.TuningSpeedUp", hdr.game_exposure.tuning_speed_up);
  SaveFloat("Hdr.Layer.Game.TuningSpeedDown", hdr.game_exposure.tuning_speed_down);
  SaveFilter("Hdr.Layer.Game.Filter", hdr.game_exposure.filters);

  SaveBool("Hdr.Layer.Adaptive.Enabled", hdr.adaptive_exposure.enabled);
  SaveFloat("Hdr.Layer.Adaptive.Midtone", hdr.adaptive_exposure.midtone);
  SaveFloat("Hdr.Layer.Adaptive.SpeedUp", hdr.adaptive_exposure.speed_up);
  SaveFloat("Hdr.Layer.Adaptive.SpeedDown", hdr.adaptive_exposure.speed_down);
  SaveFloat("Hdr.Layer.Adaptive.StrengthOverGame", hdr.adaptive_exposure.strength_over_game);
  SaveFloat("Hdr.Layer.Adaptive.MaxCorrectionStops", hdr.adaptive_exposure.max_correction_stops);
  SaveFilter("Hdr.Layer.Adaptive.Filter", hdr.adaptive_exposure.filters);

  SaveBool("Hdr.Layer.Local.Enabled", hdr.local_exposure.enabled);
  SaveFloat("Hdr.Layer.Local.Strength", hdr.local_exposure.strength);
  SaveFloat("Hdr.Layer.Local.MaxBrighten", hdr.local_exposure.max_brighten);
  SaveFloat("Hdr.Layer.Local.MaxDarken", hdr.local_exposure.max_darken);
  SaveFloat("Hdr.Layer.Local.Midtone", hdr.local_exposure.midtone);
  SaveFloat("Hdr.Layer.Local.BlockSizePx", hdr.local_exposure.block_size_px);
  SaveInt("Hdr.Layer.Local.SampleCount", hdr.local_exposure.sample_count);
  SaveFloat("Hdr.Layer.Local.EdgeSharpness", hdr.local_exposure.edge_sharpness);
  SaveBool("Hdr.Layer.Local.DepthContinuity", hdr.local_exposure.depth_continuity);
  SaveFloat("Hdr.Layer.Local.DepthEdgeThreshold", hdr.local_exposure.depth_edge_threshold);
  SaveFilter("Hdr.Layer.Local.Filter", hdr.local_exposure.filters);

  SaveFloat("Hdr.HighlightHeadroom", hdr.highlight_headroom);
  SaveFloat("Hdr.BT2446.K1", hdr.bt2446_k1);
  SaveFloat("Hdr.BT2446.K2", hdr.bt2446_k2);
  SaveFloat("Hdr.BT2446.K3", hdr.bt2446_k3);
  SaveFloat("Hdr.BT2446.K4", hdr.bt2446_k4);
  SaveFloat("Hdr.BT2446.Crosstalk", hdr.bt2446_crosstalk);
  SaveFloat("Hdr.GainMap.MinLog2", hdr.gain_min_log2);
  SaveFloat("Hdr.GainMap.MaxLog2", hdr.gain_max_log2);
}

}  // namespace reshade_dlssnr

#include "skin/settings.h"

#include <algorithm>

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadSkinSettings(SkinSettings& skin) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable",
                            skin.enable);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale",
                            skin.scale);
  int target_mode = static_cast<int>(skin.target_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Target",
                            target_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "StructureDetect.Target",
                            target_mode);
  if (target_mode < 0 || target_mode > 1) target_mode = 0;
  skin.target_mode = static_cast<DetectionTargetMode>(target_mode);
  if (skin.scale < 0.10f) skin.scale = 0.10f;
  if (skin.scale > 1.0f) skin.scale = 1.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LumaThreshold", skin.luma_threshold);
  if (skin.luma_threshold < 0.0f) skin.luma_threshold = 0.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.ChromaThreshold",
                            skin.chroma_threshold);
  if (skin.chroma_threshold < 0.0f) skin.chroma_threshold = 0.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LumaRelative", skin.luma_relative);
  if (skin.luma_relative < 0.0f) skin.luma_relative = 0.0f;
  if (skin.luma_relative > 1.0f) skin.luma_relative = 1.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.ClipThreshold", skin.clip_threshold);
  if (skin.clip_threshold < 0.5f) skin.clip_threshold = 0.5f;
  if (skin.clip_threshold > 1.0f) skin.clip_threshold = 1.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Preset",
                            skin.preset);
  if (skin.preset < 0) skin.preset = 0;
  if (skin.preset > 2) skin.preset = 2;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Intensity",
                            skin.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalTone",
                            skin.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LocalStructure", skin.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.SkinStructure", skin.skin_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.UICorrection", skin.ui_correction);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.EditStrength", skin.edit_strength);
  if (skin.edit_strength < 0.1f) skin.edit_strength = 0.1f;
  if (skin.edit_strength > 8.0f) skin.edit_strength = 8.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.MaskSmoothing", skin.mask_smoothing);
  if (skin.mask_smoothing < 0.0f) skin.mask_smoothing = 0.0f;
  if (skin.mask_smoothing > 1.0f) skin.mask_smoothing = 1.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.MaskResponse", skin.mask_response);
  if (skin.mask_response < 0.0f) skin.mask_response = 0.0f;
  if (skin.mask_response > 1.0f) skin.mask_response = 1.0f;

  int exposure_mode = static_cast<int>(skin.exposure_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.ExposureMode", exposure_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureMode", exposure_mode);
  if (exposure_mode < 0 || exposure_mode > 2) exposure_mode = 2;
  skin.exposure_mode = static_cast<DetectionExposureMode>(exposure_mode);

  // Read the old names first so existing configurations continue to load.
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrMidtone", skin.exposure_midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrRateCap", skin.exposure_rate_cap);
  int old_interval = skin.exposure_measure_interval;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrMeasureInterval", old_interval);
  skin.exposure_measure_interval = old_interval;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureMidtone", skin.exposure_midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureRateCap", skin.exposure_rate_cap);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureMeasureInterval",
                            skin.exposure_measure_interval);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.AutoLowPercent",
                            skin.exposure_auto_low_percent);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.AutoHighPercent",
                            skin.exposure_auto_high_percent);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.SpeedUp", skin.exposure_speed_up);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.SpeedDown", skin.exposure_speed_down);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalExposureStrength",
                            skin.exposure_local_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalMaxBrighten",
                            skin.exposure_local_max_brighten);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalMaxDarken",
                            skin.exposure_local_max_darken);

  skin.exposure_midtone = std::clamp(skin.exposure_midtone, 0.01f, 0.50f);
  skin.exposure_rate_cap = std::clamp(skin.exposure_rate_cap, 0.0f, 2.0f);
  skin.exposure_measure_interval = std::clamp(skin.exposure_measure_interval, 1, 32);
  skin.exposure_auto_low_percent = std::clamp(skin.exposure_auto_low_percent, 0.0f, 0.99f);
  skin.exposure_auto_high_percent = std::clamp(skin.exposure_auto_high_percent, 0.01f, 0.999f);
  if (skin.exposure_auto_high_percent <= skin.exposure_auto_low_percent) {
    skin.exposure_auto_low_percent = 0.70f;
    skin.exposure_auto_high_percent = 0.90f;
  }
  skin.exposure_speed_up = std::clamp(skin.exposure_speed_up, 0.0f, 20.0f);
  skin.exposure_speed_down = std::clamp(skin.exposure_speed_down, 0.0f, 20.0f);
  skin.exposure_local_strength = std::clamp(skin.exposure_local_strength, 0.0f, 1.0f);
  skin.exposure_local_max_brighten = std::clamp(skin.exposure_local_max_brighten, 0.0f, 4.0f);
  skin.exposure_local_max_darken = std::clamp(skin.exposure_local_max_darken, 0.0f, 4.0f);
}

void SaveSkinSettings(const SkinSettings& skin) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable",
                            skin.enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale",
                            skin.scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "StructureDetect.Target",
                            static_cast<int>(skin.target_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "StructureDetect.ExposureMode",
                            static_cast<int>(skin.exposure_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LumaThreshold", skin.luma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.ChromaThreshold",
                            skin.chroma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LumaRelative", skin.luma_relative);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.ClipThreshold", skin.clip_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Preset",
                            skin.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Intensity",
                            skin.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.LocalTone",
                            skin.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.LocalStructure", skin.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.SkinStructure", skin.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.UICorrection", skin.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.EditStrength", skin.edit_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.MaskSmoothing", skin.mask_smoothing);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.MaskResponse", skin.mask_response);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureMidtone", skin.exposure_midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureRateCap", skin.exposure_rate_cap);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.ExposureMeasureInterval",
                            skin.exposure_measure_interval);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.AutoLowPercent", skin.exposure_auto_low_percent);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.AutoHighPercent", skin.exposure_auto_high_percent);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.SpeedUp", skin.exposure_speed_up);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.SpeedDown", skin.exposure_speed_down);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalExposureStrength", skin.exposure_local_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalMaxBrighten", skin.exposure_local_max_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "StructureDetect.LocalMaxDarken", skin.exposure_local_max_darken);

}

}  // namespace reshade_dlssnr

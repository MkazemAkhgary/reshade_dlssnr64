#include "skin/settings.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadSkinSettings(SkinSettings& skin) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable",
                            skin.enable);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale",
                            skin.scale);
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

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrMidtone", skin.hdr_midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrRateCap", skin.hdr_rate_cap);
  if (skin.hdr_rate_cap < 0.0f) skin.hdr_rate_cap = 0.0f;
  if (skin.hdr_rate_cap > 2.0f) skin.hdr_rate_cap = 2.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrMeasureInterval",
                            skin.hdr_measure_interval);
  if (skin.hdr_measure_interval < 1) skin.hdr_measure_interval = 1;
  if (skin.hdr_measure_interval > 32) skin.hdr_measure_interval = 32;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrDepthFallbackFraction",
                            skin.hdr_depth_fallback_fraction);
  if (skin.hdr_depth_fallback_fraction < 0.0f)
    skin.hdr_depth_fallback_fraction = 0.0f;
  if (skin.hdr_depth_fallback_fraction > 1.0f)
    skin.hdr_depth_fallback_fraction = 1.0f;
}

void SaveSkinSettings(const SkinSettings& skin) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Enable",
                            skin.enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinDetect.Scale",
                            skin.scale);
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
                            "SkinDetect.HdrMidtone", skin.hdr_midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrRateCap", skin.hdr_rate_cap);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrMeasureInterval",
                            skin.hdr_measure_interval);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "SkinDetect.HdrDepthFallbackFraction",
                            skin.hdr_depth_fallback_fraction);
}

}  // namespace reshade_dlssnr

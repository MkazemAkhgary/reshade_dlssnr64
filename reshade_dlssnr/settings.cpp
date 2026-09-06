#include "settings.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {
Settings g_settings;
}

Settings& GetSettings() {
  return g_settings;
}

void LoadSettings() {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Intensity", g_settings.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ChromaStrength", g_settings.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "LumaStrength", g_settings.luma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "WhitePointScale", g_settings.white_point_scale);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "WorkingScale", g_settings.working_scale);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "MVScaleX", g_settings.mv_scale_x);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "MVScaleY", g_settings.mv_scale_y);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Preset", g_settings.preset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Style", g_settings.style);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "LocalStructure", g_settings.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "LocalTone", g_settings.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SkinStructure", g_settings.skin_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "AutoMask", g_settings.auto_mask);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UICorrection", g_settings.ui_correction);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "DebugView", g_settings.debug_view);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareMode", g_settings.compare_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareSplit", g_settings.compare_split);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareZoom", g_settings.compare_zoom);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareSwap", g_settings.compare_swap);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "PassthroughToneMapped", g_settings.passthrough_tone_mapped);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UseSelectedPassAsInput", g_settings.use_selected_pass_as_input);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskThreshold", g_settings.ui_mask_threshold);
}

void SaveSettings() {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Intensity", g_settings.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ChromaStrength", g_settings.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "LumaStrength", g_settings.luma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "WhitePointScale", g_settings.white_point_scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "WorkingScale", g_settings.working_scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "MVScaleX", g_settings.mv_scale_x);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "MVScaleY", g_settings.mv_scale_y);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Preset", g_settings.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Style", g_settings.style);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "LocalStructure", g_settings.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "LocalTone", g_settings.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SkinStructure", g_settings.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "AutoMask", g_settings.auto_mask);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UICorrection", g_settings.ui_correction);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "DebugView", g_settings.debug_view);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareMode", g_settings.compare_mode);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareSplit", g_settings.compare_split);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareZoom", g_settings.compare_zoom);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareSwap", g_settings.compare_swap);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "PassthroughToneMapped", g_settings.passthrough_tone_mapped);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UseSelectedPassAsInput", g_settings.use_selected_pass_as_input);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskThreshold", g_settings.ui_mask_threshold);
}

}  // namespace reshade_dlssnr

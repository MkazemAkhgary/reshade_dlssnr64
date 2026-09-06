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
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1Preset", g_settings.pass1.preset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LocalTone", g_settings.pass1.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1UICorrection", g_settings.pass1.ui_correction);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LocalStructure", g_settings.pass1.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1AutoMask", g_settings.pass1.auto_mask);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1SkinStructure", g_settings.pass1.skin_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1Intensity", g_settings.pass1.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LumaStrength", g_settings.pass1.luma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ChromaStrength", g_settings.pass1.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1WorkingScale", g_settings.pass1.working_scale);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ResidualSuppression", g_settings.pass1.residual_suppression);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ResidualThreshold", g_settings.pass1.residual_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass1WhitePointScale", g_settings.pass1.white_point_scale);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2Preset", g_settings.pass2.preset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LocalTone", g_settings.pass2.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2UICorrection", g_settings.pass2.ui_correction);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LocalStructure", g_settings.pass2.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2AutoMask", g_settings.pass2.auto_mask);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2SkinStructure", g_settings.pass2.skin_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2Intensity", g_settings.pass2.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LumaStrength", g_settings.pass2.luma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ChromaStrength", g_settings.pass2.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2WorkingScale", g_settings.pass2.working_scale);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ResidualSuppression", g_settings.pass2.residual_suppression);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ResidualThreshold", g_settings.pass2.residual_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Pass2WhitePointScale", g_settings.pass2.white_point_scale);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UseSelectedPassAsInput", g_settings.use_selected_pass_as_input);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskThreshold", g_settings.ui_mask_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskSoftness", g_settings.ui_mask_softness);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "DebugView", g_settings.debug_view);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareMode", g_settings.compare_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareSplit", g_settings.compare_split);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareZoom", g_settings.compare_zoom);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "CompareSwap", g_settings.compare_swap);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "PassthroughToneMapped", g_settings.passthrough_tone_mapped);
}

void SaveSettings() {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1Preset", g_settings.pass1.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LocalTone", g_settings.pass1.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1UICorrection", g_settings.pass1.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LocalStructure", g_settings.pass1.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1AutoMask", g_settings.pass1.auto_mask);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1SkinStructure", g_settings.pass1.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1Intensity", g_settings.pass1.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1LumaStrength", g_settings.pass1.luma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ChromaStrength", g_settings.pass1.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1WorkingScale", g_settings.pass1.working_scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ResidualSuppression", g_settings.pass1.residual_suppression);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1ResidualThreshold", g_settings.pass1.residual_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass1WhitePointScale", g_settings.pass1.white_point_scale);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2Preset", g_settings.pass2.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LocalTone", g_settings.pass2.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2UICorrection", g_settings.pass2.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LocalStructure", g_settings.pass2.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2AutoMask", g_settings.pass2.auto_mask);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2SkinStructure", g_settings.pass2.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2Intensity", g_settings.pass2.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2LumaStrength", g_settings.pass2.luma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ChromaStrength", g_settings.pass2.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2WorkingScale", g_settings.pass2.working_scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ResidualSuppression", g_settings.pass2.residual_suppression);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2ResidualThreshold", g_settings.pass2.residual_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Pass2WhitePointScale", g_settings.pass2.white_point_scale);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UseSelectedPassAsInput", g_settings.use_selected_pass_as_input);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskThreshold", g_settings.ui_mask_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UIMaskSoftness", g_settings.ui_mask_softness);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "DebugView", g_settings.debug_view);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareMode", g_settings.compare_mode);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareSplit", g_settings.compare_split);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareZoom", g_settings.compare_zoom);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "CompareSwap", g_settings.compare_swap);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "PassthroughToneMapped", g_settings.passthrough_tone_mapped);
}

}  // namespace reshade_dlssnr

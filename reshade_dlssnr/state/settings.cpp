#include "settings.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {
Settings g_settings;
}

Settings& GetSettings() {
  return g_settings;
}

namespace {

template <typename T>
void LoadPassSettings(const char* prefix, PassSettings& pass) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "Preset").c_str(),
                            pass.preset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LocalTone").c_str(),
                            pass.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "UICorrection").c_str(),
                            pass.ui_correction);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LocalStructure").c_str(),
                            pass.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "AutoMask").c_str(),
                            pass.auto_mask);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "SkinStructure").c_str(),
                            pass.skin_structure);
  if (pass.skin_structure == 0.0f) pass.skin_structure = 1.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "Intensity").c_str(),
                            pass.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LumaStrength").c_str(),
                            pass.luma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ChromaStrength").c_str(),
                            pass.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "WorkingScale").c_str(),
                            pass.working_scale);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ResidualSuppression").c_str(),
                            pass.residual_suppression);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ResidualThreshold").c_str(),
                            pass.residual_threshold);
}

void SavePassSettings(const char* prefix, const PassSettings& pass) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "Preset").c_str(),
                            pass.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LocalTone").c_str(),
                            pass.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "UICorrection").c_str(),
                            pass.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LocalStructure").c_str(),
                            pass.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "AutoMask").c_str(),
                            pass.auto_mask);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "SkinStructure").c_str(),
                            pass.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "Intensity").c_str(),
                            pass.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "LumaStrength").c_str(),
                            pass.luma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ChromaStrength").c_str(),
                            pass.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "WorkingScale").c_str(),
                            pass.working_scale);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ResidualSuppression").c_str(),
                            pass.residual_suppression);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (std::string(prefix) + "ResidualThreshold").c_str(),
                            pass.residual_threshold);
}

}  // namespace

void LoadSettings() {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);

  LoadPassSettings("Pass1", g_settings.pass1);
  LoadPassSettings("Pass2", g_settings.pass2);
}

void SaveSettings() {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);

  SavePassSettings("Pass1", g_settings.pass1);
  SavePassSettings("Pass2", g_settings.pass2);
}

}  // namespace reshade_dlssnr

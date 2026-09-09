#include "settings.h"

#include <reshade.hpp>

#include <string>

#include "shader_params.h"

namespace reshade_dlssnr {

namespace {
Settings g_settings;
}

Settings& GetSettings() {
  return g_settings;
}

namespace {

void LoadPassSettings(const char* prefix, PassSettings& pass) {
  const std::string pref(prefix);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Preset").c_str(), pass.preset);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "LocalTone").c_str(), pass.local_tone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "UICorrection").c_str(), pass.ui_correction);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "LocalStructure").c_str(), pass.local_structure);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "AutoMask").c_str(), pass.auto_mask);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "SkinStructure").c_str(), pass.skin_structure);
  if (pass.skin_structure == 0.0f) pass.skin_structure = 1.0f;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Intensity").c_str(), pass.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "WorkingScale").c_str(), pass.working_scale);

  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    float v = kShaderParams[i].default_value;
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + kShaderParams[i].name).c_str(), v);
    pass.params[i] = v;
  }
}

void SavePassSettings(const char* prefix, const PassSettings& pass) {
  const std::string pref(prefix);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Preset").c_str(), pass.preset);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "LocalTone").c_str(), pass.local_tone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "UICorrection").c_str(), pass.ui_correction);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "LocalStructure").c_str(), pass.local_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "AutoMask").c_str(), pass.auto_mask);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "SkinStructure").c_str(), pass.skin_structure);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Intensity").c_str(), pass.intensity);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "WorkingScale").c_str(), pass.working_scale);

  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + kShaderParams[i].name).c_str(),
                              pass.params[i]);
  }
}

}  // namespace

void LoadSettings() {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ThirdPassEnabled", g_settings.third_pass_enabled);

  LoadPassSettings("Pass1", g_settings.pass1);
  LoadPassSettings("Pass2", g_settings.pass2);
  LoadPassSettings("Pass3", g_settings.pass3);
}

void SaveSettings() {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled", g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling", g_settings.only_super_sampling);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "SecondPassEnabled", g_settings.second_pass_enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ThirdPassEnabled", g_settings.third_pass_enabled);

  SavePassSettings("Pass1", g_settings.pass1);
  SavePassSettings("Pass2", g_settings.pass2);
  SavePassSettings("Pass3", g_settings.pass3);
}

}  // namespace reshade_dlssnr

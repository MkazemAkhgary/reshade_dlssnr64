#include "settings.h"

#include <reshade.hpp>

#include <string>

#include "compute/shader_params.h"

namespace reshade_dlssnr {

namespace {
Settings g_settings;
}

Settings& GetSettings() {
  return g_settings;
}

namespace {

std::string PassPrefix(uint32_t index) {
  return "Pass" + std::to_string(index + 1);
}

std::string PassEnabledKey(uint32_t index) {
  return "Pass" + std::to_string(index + 1) + "Enabled";
}

void LoadPassSettings(uint32_t index, PassSettings& pass) {
  const std::string pref = PassPrefix(index);
  const bool inherit_capable = index > 0;

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

  int injection_stage = static_cast<int>(pass.injection_stage);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "InjectionStage").c_str(), injection_stage);
  if (injection_stage < 0 || injection_stage > 1) injection_stage = 1;
  pass.injection_stage = static_cast<InjectionStage>(injection_stage);

  if (inherit_capable) {
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.WorkingScale").c_str(),
                              pass.inherit_working_scale);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.Intensity").c_str(),
                              pass.inherit_intensity);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.LocalTone").c_str(),
                              pass.inherit_local_tone);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.LocalStructure").c_str(),
                              pass.inherit_local_structure);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.SkinStructure").c_str(),
                              pass.inherit_skin_structure);
  }

  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    float v = kShaderParams[i].default_value;
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + kShaderParams[i].name).c_str(), v);
    pass.params[i] = v;

    if (inherit_capable) {
      reshade::get_config_value(
          nullptr, "RESHADE_DLSSNR",
          (pref + "Inherit." + kShaderParams[i].name).c_str(),
          pass.inherit_params[i]);
    }
  }
}

void SavePassSettings(uint32_t index, const PassSettings& pass) {
  const std::string pref = PassPrefix(index);
  const bool inherit_capable = index > 0;

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

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "InjectionStage").c_str(),
                            static_cast<int>(pass.injection_stage));

  if (inherit_capable) {
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.WorkingScale").c_str(),
                              pass.inherit_working_scale);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.Intensity").c_str(),
                              pass.inherit_intensity);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.LocalTone").c_str(),
                              pass.inherit_local_tone);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.LocalStructure").c_str(),
                              pass.inherit_local_structure);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + "Inherit.SkinStructure").c_str(),
                              pass.inherit_skin_structure);
  }

  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                              (pref + kShaderParams[i].name).c_str(),
                              pass.params[i]);

    if (inherit_capable) {
      reshade::set_config_value(
          nullptr, "RESHADE_DLSSNR",
          (pref + "Inherit." + kShaderParams[i].name).c_str(),
          pass.inherit_params[i]);
    }
  }
}

}  // namespace

void LoadSettings() {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Enabled",
                            g_settings.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling",
                            g_settings.only_super_sampling);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "DebugView",
                            g_settings.debug_view);
  if (g_settings.debug_view < 0) g_settings.debug_view = 0;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            g_settings.hdr_enable);
  int hdr_format = static_cast<int>(g_settings.hdr_format);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            hdr_format);
  if (hdr_format < 0 || hdr_format > 3) hdr_format = 0;
  g_settings.hdr_format = static_cast<HdrFormat>(hdr_format);

  int chroma_mode = static_cast<int>(g_settings.hdr_chroma_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.ChromaMode",
                            chroma_mode);
  if (chroma_mode < 0 || chroma_mode > 1) chroma_mode = 0;
  g_settings.hdr_chroma_mode = static_cast<ChromaEditMode>(chroma_mode);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Responsiveness",
                            g_settings.hdr_responsiveness);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            g_settings.hdr_midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            g_settings.hdr_white_percentile);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.CurveWhite",
                            g_settings.hdr_curve_white);
  // Clamp to the slider's range. Values outside it are almost certainly
  // from an earlier build; snap to the current default.
  if (g_settings.hdr_curve_white > 16.0f ||
      g_settings.hdr_curve_white < 0.25f) {
    g_settings.hdr_curve_white = 8.0f;
  }
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.EditStrength",
                            g_settings.hdr_edit_strength);

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string key = PassEnabledKey(i);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.pass_enabled[i]);
    LoadPassSettings(i, g_settings.passes[i]);
  }
  g_settings.pass_enabled[0] = true;

  // Pass 0 is the inheritance root; its inherit flags are meaningless.
  g_settings.passes[0].inherit_working_scale = false;
  g_settings.passes[0].inherit_intensity = false;
  g_settings.passes[0].inherit_local_tone = false;
  g_settings.passes[0].inherit_local_structure = false;
  g_settings.passes[0].inherit_skin_structure = false;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    g_settings.passes[0].inherit_params[i] = false;
  }

  // Enforce constraint: once a pass is at Present, all subsequent are too.
  for (uint32_t i = 1; i < kMaxPasses; ++i) {
    if (g_settings.passes[i - 1].injection_stage == InjectionStage::Present) {
      g_settings.passes[i].injection_stage = InjectionStage::Present;
    }
  }
}

void SaveSettings() {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled",
                            g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling",
                            g_settings.only_super_sampling);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "DebugView",
                            g_settings.debug_view);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            g_settings.hdr_enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            static_cast<int>(g_settings.hdr_format));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.ChromaMode",
                            static_cast<int>(g_settings.hdr_chroma_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Responsiveness",
                            g_settings.hdr_responsiveness);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            g_settings.hdr_midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            g_settings.hdr_white_percentile);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.CurveWhite",
                            g_settings.hdr_curve_white);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.EditStrength",
                            g_settings.hdr_edit_strength);

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string key = PassEnabledKey(i);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.pass_enabled[i]);
    SavePassSettings(i, g_settings.passes[i]);
  }
}

PassSettings ResolvePassSettings(uint32_t index) {
  if (index >= kMaxPasses) return {};

  PassSettings resolved = g_settings.passes[index];
  if (index == 0) return resolved;

  const PassSettings prev = ResolvePassSettings(index - 1);

  if (resolved.inherit_working_scale) {
    resolved.working_scale = prev.working_scale;
  }
  if (resolved.inherit_intensity) {
    resolved.intensity = prev.intensity;
  }
  if (resolved.inherit_local_tone) {
    resolved.local_tone = prev.local_tone;
  }
  if (resolved.inherit_local_structure) {
    resolved.local_structure = prev.local_structure;
  }
  if (resolved.inherit_skin_structure) {
    resolved.skin_structure = prev.skin_structure;
  }
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    if (resolved.inherit_params[i]) {
      resolved.params[i] = prev.params[i];
    }
  }

  return resolved;
}

}  // namespace reshade_dlssnr

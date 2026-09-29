#include "settings.h"

#include <reshade.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "compute/shader_params.h"
#include "settings/compose.h"
#include "settings/grade.h"
#include "settings/trace.h"

namespace reshade_dlssnr {

// The parameter table is written into PassSettings::params by index, and that
// index is not bounds checked. This is the check that the storage is big
// enough for the table.
static_assert(kShaderParamCount <= kShaderParamCapacity,
              "kShaderParams has outgrown PassSettings storage - raise "
              "kShaderParamCapacity in settings.h");

namespace {
Settings g_settings;

// Derived from g_settings.passes and the inherit flags. Kept in sync by
// RecomputeResolved(), which is called from LoadSettings and SaveSettings.
PassSettings g_resolved[kMaxPasses];

void RecomputeResolved() {
  g_resolved[0] = g_settings.passes[0];
  for (uint32_t i = 1; i < kMaxPasses; ++i) {
    PassSettings r = g_settings.passes[i];
    const PassSettings& prev = g_resolved[i - 1];

    if (r.inherit_working_scale) r.working_scale = prev.working_scale;
    if (r.inherit_intensity) r.intensity = prev.intensity;
    if (r.inherit_local_tone) r.local_tone = prev.local_tone;
    if (r.inherit_local_structure) r.local_structure = prev.local_structure;
    if (r.inherit_skin_structure) r.skin_structure = prev.skin_structure;
    for (uint32_t j = 0; j < kShaderParamCount; ++j) {
      if (r.inherit_params[j]) r.params[j] = prev.params[j];
    }

    g_resolved[i] = r;
  }
}

}  // namespace

Settings& GetSettings() {
  return g_settings;
}

const PassSettings& GetResolvedPass(uint32_t index) {
  if (index >= kMaxPasses) {
    static const PassSettings kEmpty;
    return kEmpty;
  }
  return g_resolved[index];
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
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Intensity").c_str(), pass.intensity);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "WorkingScale").c_str(), pass.working_scale);

  int injection_stage = static_cast<int>(pass.injection_stage);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "InjectionStage").c_str(), injection_stage);
  if (injection_stage < 0 || injection_stage > 1) injection_stage = 1;
  pass.injection_stage = static_cast<InjectionStage>(injection_stage);

  LoadResidual(pref + "Residual.", pass.residual);

  size_t custom_name_size = sizeof(pass.custom_name);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "CustomName").c_str(),
                            pass.custom_name, &custom_name_size);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Rcas.Enabled").c_str(),
                            pass.rcas_enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Rcas.Strength").c_str(),
                            pass.rcas_strength);
  if (pass.rcas_strength < 0.0f) pass.rcas_strength = 0.0f;
  if (pass.rcas_strength > 1.0f) pass.rcas_strength = 1.0f;

  LoadGrade(pref + "Grade.Input.", pass.grade_input);
  LoadGrade(pref + "Grade.Output.", pass.grade_output);

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

  SaveResidual(pref + "Residual.", pass.residual);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "CustomName").c_str(),
                            pass.custom_name);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Rcas.Enabled").c_str(),
                            pass.rcas_enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (pref + "Rcas.Strength").c_str(),
                            pass.rcas_strength);

  SaveGrade(pref + "Grade.Input.", pass.grade_input);
  SaveGrade(pref + "Grade.Output.", pass.grade_output);

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
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ShowTooltips",
                            g_settings.show_tooltips);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "DebugView",
                            g_settings.debug_view);
  if (g_settings.debug_view < 0) g_settings.debug_view = 0;

  LoadHdrSettings(g_settings.hdr);

  LoadSkinSettings(g_settings.skin);

  LoadGrade("Grade.Backbuffer.", g_settings.grade_backbuffer);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.StatusOpen",
                            g_settings.ui_status_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrOpen",
                            g_settings.ui_hdr_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.GradeOpen",
                            g_settings.ui_grade_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrDiagnosticsOpen",
                            g_settings.ui_hdr_diagnostics_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.SkinDetectOpen",
                            g_settings.ui_skin_detect_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectModelOpen",
                            g_settings.ui_skin_detect_model_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectMaskOpen",
                            g_settings.ui_skin_detect_mask_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectHdrOpen",
                            g_settings.ui_skin_detect_hdr_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.ModelOpen",
                            g_settings.ui_model_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.ResidualOpen",
                            g_settings.ui_residual_open);
  for (uint32_t j = 0; j < kShaderParamCount; ++j) {
    const std::string key = "UI.CategoryOpen" + std::to_string(j);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.ui_category_open[j]);
  }

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string key = PassEnabledKey(i);
    reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.pass_enabled[i]);
    LoadPassSettings(i, g_settings.passes[i]);
  }

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

  // Apply inheritance so g_resolved reflects the loaded state.
  RecomputeResolved();
  TraceSettings(g_settings, g_resolved);
}

void SaveSettings() {
  // Refresh the resolved view before persisting so any caller that reads
  // GetResolvedPass() after a save sees the current state.
  RecomputeResolved();
  TraceSettings(g_settings, g_resolved);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Enabled",
                            g_settings.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "OnlySuperSampling",
                            g_settings.only_super_sampling);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ShowTooltips",
                            g_settings.show_tooltips);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "DebugView",
                            g_settings.debug_view);

  SaveHdrSettings(g_settings.hdr);

  SaveSkinSettings(g_settings.skin);

  SaveGrade("Grade.Backbuffer.", g_settings.grade_backbuffer);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.StatusOpen",
                            g_settings.ui_status_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrOpen",
                            g_settings.ui_hdr_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.GradeOpen",
                            g_settings.ui_grade_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrDiagnosticsOpen",
                            g_settings.ui_hdr_diagnostics_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.SkinDetectOpen",
                            g_settings.ui_skin_detect_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectModelOpen",
                            g_settings.ui_skin_detect_model_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectMaskOpen",
                            g_settings.ui_skin_detect_mask_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            "UI.SkinDetectHdrOpen",
                            g_settings.ui_skin_detect_hdr_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.ModelOpen",
                            g_settings.ui_model_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.ResidualOpen",
                            g_settings.ui_residual_open);
  for (uint32_t j = 0; j < kShaderParamCount; ++j) {
    const std::string key = "UI.CategoryOpen" + std::to_string(j);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.ui_category_open[j]);
  }

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string key = PassEnabledKey(i);
    reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                              g_settings.pass_enabled[i]);
    SavePassSettings(i, g_settings.passes[i]);
  }
}

}  // namespace reshade_dlssnr

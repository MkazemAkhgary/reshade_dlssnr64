#include "settings.h"

#include <reshade.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "compute/shader_params.h"
#include "pipeline_trace.h"
#include "settings/grade.h"

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

// Residual edit persistence. One value today; kept beside the grade pair so
// the per-pass key layout stays in one place.
void LoadResidual(const std::string& prefix, ResidualSettings& residual) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            residual.chroma_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditGamma").c_str(),
                            residual.edit_gamma);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HighlightProtect").c_str(),
                            residual.highlight_protect);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ShadowProtect").c_str(),
                            residual.shadow_protect);
  if (residual.edit_strength < 0.0f) residual.edit_strength = 0.0f;
  if (residual.edit_strength > 2.0f) residual.edit_strength = 2.0f;
  if (residual.chroma_strength < 0.0f) residual.chroma_strength = 0.0f;
  if (residual.chroma_strength > 1.0f) residual.chroma_strength = 1.0f;
  if (residual.edit_gamma < 0.25f) residual.edit_gamma = 0.25f;
  if (residual.edit_gamma > 4.0f) residual.edit_gamma = 4.0f;
  if (residual.highlight_protect < 0.0f) residual.highlight_protect = 0.0f;
  if (residual.highlight_protect > 1.0f) residual.highlight_protect = 1.0f;
  if (residual.shadow_protect < 0.0f) residual.shadow_protect = 0.0f;
  if (residual.shadow_protect > 1.0f) residual.shadow_protect = 1.0f;

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioEps").c_str(),
                            residual.ratio_eps);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioBrighten").c_str(),
                            residual.ratio_brighten);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioDarken").c_str(),
                            residual.ratio_darken);
  if (residual.ratio_eps < 1e-6f) residual.ratio_eps = 1e-6f;
  if (residual.ratio_eps > 0.5f) residual.ratio_eps = 0.5f;
  if (residual.ratio_brighten < 1.0f) residual.ratio_brighten = 1.0f;
  if (residual.ratio_brighten > 256.0f) residual.ratio_brighten = 256.0f;
  if (residual.ratio_darken > 1.0f) residual.ratio_darken = 1.0f;
  if (residual.ratio_darken < 1.0f / 256.0f)
    residual.ratio_darken = 1.0f / 256.0f;
}

void SaveResidual(const std::string& prefix, const ResidualSettings& residual) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaStrength").c_str(),
                            residual.chroma_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditGamma").c_str(),
                            residual.edit_gamma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "HighlightProtect").c_str(),
                            residual.highlight_protect);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ShadowProtect").c_str(),
                            residual.shadow_protect);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioEps").c_str(),
                            residual.ratio_eps);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioBrighten").c_str(),
                            residual.ratio_brighten);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "RatioDarken").c_str(),
                            residual.ratio_darken);
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

namespace {

// ---------------------------------------------------------------------------
// Settings trace
// ---------------------------------------------------------------------------
//
// Every setting is serialized to a name/value pair in a fixed order, and two
// snapshots from the same build compare element by element. The whole block is
// behind trace::Enabled(), so nothing here is built unless DLSSNR_TRACE is set.
struct TraceField {
  std::string name;
  std::string value;
  // Set on resolved[] fields only: the value the field has before
  // inheritance. Used to keep the baseline dump from repeating every pass.
  std::string raw_value;
  bool resolved = false;
};

using Snapshot = std::vector<TraceField>;

std::string FloatText(float v) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4g", v);
  return std::string(buffer);
}

const char* BoolText(bool v) { return v ? "1" : "0"; }

const char* StageText(InjectionStage stage) {
  return stage == InjectionStage::Present ? "Present" : "PostDLSS";
}

const char* HdrFormatText(HdrFormat format) {
  switch (format) {
    case HdrFormat::Sdr: return "Sdr";
    case HdrFormat::ScRgb: return "ScRgb";
    case HdrFormat::Hdr10: return "Hdr10";
    case HdrFormat::Auto:
    default: return "Auto";
  }
}

const char* LumaModeText(GradeLumaMode mode) {
  switch (mode) {
    case GradeLumaMode::Gain: return "Gain";
    case GradeLumaMode::Gamma: return "Gamma";
    case GradeLumaMode::Lift: return "Lift";
    case GradeLumaMode::Soft:
    default: return "Soft";
  }
}

const char* ChromaModeText(GradeChromaMode mode) {
  switch (mode) {
    case GradeChromaMode::Saturation: return "Saturation";
    case GradeChromaMode::Vibrance: return "Vibrance";
    case GradeChromaMode::ChromaGain:
    default: return "ChromaGain";
  }
}

void Append(Snapshot& snap, const std::string& name, std::string value) {
  snap.push_back(TraceField{std::move(name), std::move(value), std::string(), false});
}

void AppendGrade(Snapshot& snap, const std::string& prefix,
                 const GradeSettings& grade) {
  Append(snap, prefix + ".enabled", BoolText(grade.enabled));
  Append(snap, prefix + ".luma", FloatText(grade.luma));
  Append(snap, prefix + ".chroma", FloatText(grade.chroma));
  Append(snap, prefix + ".luma_mode", LumaModeText(grade.luma_mode));
  Append(snap, prefix + ".chroma_mode", ChromaModeText(grade.chroma_mode));
  Append(snap, prefix + ".black_point", FloatText(grade.black_point));
  Append(snap, prefix + ".white_point", FloatText(grade.white_point));
  Append(snap, prefix + ".contrast", FloatText(grade.contrast));
  Append(snap, prefix + ".shadows", FloatText(grade.shadows));
  Append(snap, prefix + ".highlights", FloatText(grade.highlights));
}

void AppendPass(Snapshot& snap, const std::string& prefix,
                const PassSettings& pass) {
  Append(snap, prefix + ".preset", std::to_string(pass.preset));
  Append(snap, prefix + ".local_tone", FloatText(pass.local_tone));
  Append(snap, prefix + ".ui_correction", BoolText(pass.ui_correction));
  Append(snap, prefix + ".local_structure", FloatText(pass.local_structure));
  Append(snap, prefix + ".auto_mask", BoolText(pass.auto_mask));
  Append(snap, prefix + ".skin_structure", FloatText(pass.skin_structure));
  Append(snap, prefix + ".intensity", FloatText(pass.intensity));
  Append(snap, prefix + ".working_scale", FloatText(pass.working_scale));
  Append(snap, prefix + ".injection_stage", StageText(pass.injection_stage));
  Append(snap, prefix + ".residual.edit_strength",
         FloatText(pass.residual.edit_strength));
  Append(snap, prefix + ".residual.chroma_strength",
         FloatText(pass.residual.chroma_strength));
  Append(snap, prefix + ".residual.edit_gamma",
         FloatText(pass.residual.edit_gamma));
  Append(snap, prefix + ".residual.highlight_protect",
         FloatText(pass.residual.highlight_protect));
  Append(snap, prefix + ".residual.shadow_protect",
         FloatText(pass.residual.shadow_protect));
  Append(snap, prefix + ".residual.ratio_eps",
         FloatText(pass.residual.ratio_eps));
  Append(snap, prefix + ".residual.ratio_brighten",
         FloatText(pass.residual.ratio_brighten));
  Append(snap, prefix + ".residual.ratio_darken",
         FloatText(pass.residual.ratio_darken));
  AppendGrade(snap, prefix + ".grade_input", pass.grade_input);
  AppendGrade(snap, prefix + ".grade_output", pass.grade_output);
  Append(snap, prefix + ".inherit.working_scale",
         BoolText(pass.inherit_working_scale));
  Append(snap, prefix + ".inherit.intensity", BoolText(pass.inherit_intensity));
  Append(snap, prefix + ".inherit.local_tone",
         BoolText(pass.inherit_local_tone));
  Append(snap, prefix + ".inherit.local_structure",
         BoolText(pass.inherit_local_structure));
  Append(snap, prefix + ".inherit.skin_structure",
         BoolText(pass.inherit_skin_structure));
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    const std::string name = kShaderParams[i].name;
    Append(snap, prefix + ".params." + name, FloatText(pass.params[i]));
    Append(snap, prefix + ".inherit_params." + name,
           BoolText(pass.inherit_params[i]));
  }
}

// UI-only state (collapsed sections, tooltips) is left out: it changes nothing
// on the GPU, and opening a section would otherwise log a line every time.
Snapshot BuildSnapshot(const Settings& settings) {
  Snapshot snap;
  Append(snap, "enabled", BoolText(settings.enabled));
  Append(snap, "only_super_sampling", BoolText(settings.only_super_sampling));
  Append(snap, "debug_view", std::to_string(settings.debug_view));
  Append(snap, "active_pass", std::to_string(settings.active_pass));
  Append(snap, "hdr_enable", BoolText(settings.hdr.enable));
  Append(snap, "hdr_format", HdrFormatText(settings.hdr.format));
  Append(snap, "hdr_measure_interval",
         std::to_string(settings.hdr.measure_interval));
  Append(snap, "hdr_midtone", FloatText(settings.hdr.midtone));
  Append(snap, "hdr_white_percentile", FloatText(settings.hdr.white_percentile));
  Append(snap, "hdr_highlight_headroom",
         FloatText(settings.hdr.highlight_headroom));
  Append(snap, "hdr_rate_cap", FloatText(settings.hdr.rate_cap));
  Append(snap, "hdr_skin_fraction_threshold",
         FloatText(settings.hdr.skin_fraction_threshold));
  Append(snap, "hdr_skin_priority", FloatText(settings.hdr.skin_priority));
  Append(snap, "hdr_depth_fallback_fraction",
         FloatText(settings.hdr.depth_fallback_fraction));
  Append(snap, "skin_detect_enable", BoolText(settings.skin.enable));
  Append(snap, "skin_detect_scale", FloatText(settings.skin.scale));
  Append(snap, "skin_detect_luma_threshold",
         FloatText(settings.skin.luma_threshold));
  Append(snap, "skin_detect_luma_relative",
         FloatText(settings.skin.luma_relative));
  Append(snap, "skin_detect_chroma_threshold",
         FloatText(settings.skin.chroma_threshold));
  Append(snap, "skin_detect_clip_threshold",
         FloatText(settings.skin.clip_threshold));
  Append(snap, "skin_detect_preset",
         std::to_string(settings.skin.preset));
  Append(snap, "skin_detect_intensity",
         FloatText(settings.skin.intensity));
  Append(snap, "skin_detect_local_tone",
         FloatText(settings.skin.local_tone));
  Append(snap, "skin_detect_local_structure",
         FloatText(settings.skin.local_structure));
  Append(snap, "skin_detect_skin_structure",
         FloatText(settings.skin.skin_structure));
  Append(snap, "skin_detect_ui_correction",
         BoolText(settings.skin.ui_correction));
  Append(snap, "skin_detect_edit_strength",
         FloatText(settings.skin.edit_strength));
  Append(snap, "skin_detect_mask_smoothing",
         FloatText(settings.skin.mask_smoothing));
  Append(snap, "skin_detect_mask_response",
         FloatText(settings.skin.mask_response));
  Append(snap, "skin_detect_hdr_midtone",
         FloatText(settings.skin.hdr_midtone));
  Append(snap, "skin_detect_hdr_rate_cap",
         FloatText(settings.skin.hdr_rate_cap));
  Append(snap, "skin_detect_hdr_measure_interval",
         std::to_string(settings.skin.hdr_measure_interval));
  Append(snap, "skin_detect_hdr_depth_fallback_fraction",
         FloatText(settings.skin.hdr_depth_fallback_fraction));
  AppendGrade(snap, "grade_backbuffer", settings.grade_backbuffer);

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string prefix = "pass[" + std::to_string(i) + "]";
    Append(snap, prefix + ".enabled", BoolText(settings.pass_enabled[i]));

    Snapshot pass;
    AppendPass(pass, prefix, settings.passes[i]);
    // The resolved view is what the runtime reads. It is traced next to its
    // pass with the pre-inheritance value attached, so the dump can skip the
    // fields inheritance did not touch.
    Snapshot resolved;
    AppendPass(resolved, "resolved[" + std::to_string(i) + "]", g_resolved[i]);
    for (size_t j = 0; j < resolved.size() && j < pass.size(); ++j) {
      resolved[j].resolved = true;
      resolved[j].raw_value = pass[j].value;
    }

    snap.insert(snap.end(), pass.begin(), pass.end());
    snap.insert(snap.end(), resolved.begin(), resolved.end());
  }
  return snap;
}

Snapshot g_traced;
bool g_traced_valid = false;

void EmitSnapshot(const Snapshot& previous, const Snapshot& current,
                  bool baseline) {
  const bool comparable = !baseline && previous.size() == current.size();
  for (size_t i = 0; i < current.size(); ++i) {
    const TraceField& field = current[i];
    if (!comparable) {
      if (field.resolved && field.raw_value == field.value) continue;
      TRACE_CFG("init %s %s", field.name.c_str(), field.value.c_str());
      continue;
    }
    if (previous[i].value == field.value) continue;
    TRACE_CFG("set %s %s -> %s", field.name.c_str(), previous[i].value.c_str(),
              field.value.c_str());
  }
}

// Called from LoadSettings (baseline) and SaveSettings (delta). The UI writes
// into the settings in place, so a save is the only point where a change is
// complete enough to be worth recording.
void TraceSettings(const Settings& settings) {
  if (!trace::Enabled()) return;
  Snapshot current = BuildSnapshot(settings);
  EmitSnapshot(g_traced, current, !g_traced_valid);
  g_traced = std::move(current);
  g_traced_valid = true;
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
  TraceSettings(g_settings);
}

void SaveSettings() {
  // Refresh the resolved view before persisting so any caller that reads
  // GetResolvedPass() after a save sees the current state.
  RecomputeResolved();
  TraceSettings(g_settings);

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

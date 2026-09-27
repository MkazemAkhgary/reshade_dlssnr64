#include "settings.h"

#include <reshade.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "compute/shader_params.h"
#include "pipeline_trace.h"

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

float ClampRange(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float ClampGradeAmount(float v) {
  return ClampRange(v, -1.0f, 1.0f);
}

// Grade persistence, shared by the per-pass grades and the backbuffer one.
// `prefix` names the grade, e.g. "Pass1Grade.Input.".
//
// Modes are stored as ints and range-checked on the way in, so a config
// written by a build with a different mode list cannot index past the end of
// the combo's item array.
void LoadGrade(const std::string& prefix, GradeSettings& grade) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Enabled").c_str(), grade.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", (prefix + "Luma").c_str(),
                            grade.luma);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Chroma").c_str(), grade.chroma);

  int luma_mode = static_cast<int>(grade.luma_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "LumaMode").c_str(), luma_mode);
  if (luma_mode < 0 || luma_mode > static_cast<int>(GradeLumaMode::Soft)) {
    luma_mode = static_cast<int>(GradeLumaMode::Gamma);
  }
  grade.luma_mode = static_cast<GradeLumaMode>(luma_mode);

  int chroma_mode = static_cast<int>(grade.chroma_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaMode").c_str(), chroma_mode);
  if (chroma_mode < 0 ||
      chroma_mode > static_cast<int>(GradeChromaMode::ChromaGain)) {
    chroma_mode = static_cast<int>(GradeChromaMode::Saturation);
  }
  grade.chroma_mode = static_cast<GradeChromaMode>(chroma_mode);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "BlackPoint").c_str(), grade.black_point);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "WhitePoint").c_str(), grade.white_point);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Contrast").c_str(), grade.contrast);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Shadows").c_str(), grade.shadows);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Highlights").c_str(), grade.highlights);

  grade.luma = ClampGradeAmount(grade.luma);
  grade.chroma = ClampGradeAmount(grade.chroma);
  grade.black_point =
      ClampRange(grade.black_point, kGradeBlackPointMin, kGradeBlackPointMax);
  grade.white_point =
      ClampRange(grade.white_point, kGradeWhitePointMin, kGradeWhitePointMax);
  grade.contrast = ClampGradeAmount(grade.contrast);
  grade.shadows = ClampGradeAmount(grade.shadows);
  grade.highlights = ClampGradeAmount(grade.highlights);
}

void SaveGrade(const std::string& prefix, const GradeSettings& grade) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Enabled").c_str(), grade.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", (prefix + "Luma").c_str(),
                            grade.luma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Chroma").c_str(), grade.chroma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "LumaMode").c_str(),
                            static_cast<int>(grade.luma_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "ChromaMode").c_str(),
                            static_cast<int>(grade.chroma_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "BlackPoint").c_str(), grade.black_point);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "WhitePoint").c_str(), grade.white_point);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Contrast").c_str(), grade.contrast);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Shadows").c_str(), grade.shadows);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "Highlights").c_str(), grade.highlights);
}

// Residual edit persistence. One value today; kept beside the grade pair so
// the per-pass key layout stays in one place.
void LoadResidual(const std::string& prefix, ResidualSettings& residual) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
  if (residual.edit_strength < 0.0f) residual.edit_strength = 0.0f;
  if (residual.edit_strength > 2.0f) residual.edit_strength = 2.0f;
}

void SaveResidual(const std::string& prefix, const ResidualSettings& residual) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (prefix + "EditStrength").c_str(),
                            residual.edit_strength);
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
  Append(snap, "hdr_enable", BoolText(settings.hdr_enable));
  Append(snap, "hdr_format", HdrFormatText(settings.hdr_format));
  Append(snap, "hdr_measure_interval",
         std::to_string(settings.hdr_measure_interval));
  Append(snap, "hdr_midtone", FloatText(settings.hdr_midtone));
  Append(snap, "hdr_white_percentile", FloatText(settings.hdr_white_percentile));
  Append(snap, "hdr_highlight_headroom",
         FloatText(settings.hdr_highlight_headroom));
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

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            g_settings.hdr_enable);
  int hdr_format = static_cast<int>(g_settings.hdr_format);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            hdr_format);
  if (hdr_format < 0 || hdr_format > 3) hdr_format = 0;
  g_settings.hdr_format = static_cast<HdrFormat>(hdr_format);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.MeasureInterval",
                            g_settings.hdr_measure_interval);
  if (g_settings.hdr_measure_interval < 1) g_settings.hdr_measure_interval = 1;
  if (g_settings.hdr_measure_interval > 32) g_settings.hdr_measure_interval = 32;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            g_settings.hdr_midtone);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            g_settings.hdr_white_percentile);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            g_settings.hdr_highlight_headroom);
  // Clamp to the slider's range.
  if (g_settings.hdr_highlight_headroom > 16.0f ||
      g_settings.hdr_highlight_headroom < 0.25f) {
    g_settings.hdr_highlight_headroom = 8.0f;
  }

  LoadGrade("Grade.Backbuffer.", g_settings.grade_backbuffer);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.StatusOpen",
                            g_settings.ui_status_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrOpen",
                            g_settings.ui_hdr_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.GradeOpen",
                            g_settings.ui_grade_open);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrDiagnosticsOpen",
                            g_settings.ui_hdr_diagnostics_open);
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

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Enable",
                            g_settings.hdr_enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Format",
                            static_cast<int>(g_settings.hdr_format));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.MeasureInterval",
                            g_settings.hdr_measure_interval);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.Midtone",
                            g_settings.hdr_midtone);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.WhitePercentile",
                            g_settings.hdr_white_percentile);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "Hdr.HighlightHeadroom",
                            g_settings.hdr_highlight_headroom);

  SaveGrade("Grade.Backbuffer.", g_settings.grade_backbuffer);

  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.StatusOpen",
                            g_settings.ui_status_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrOpen",
                            g_settings.ui_hdr_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.GradeOpen",
                            g_settings.ui_grade_open);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "UI.HdrDiagnosticsOpen",
                            g_settings.ui_hdr_diagnostics_open);
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

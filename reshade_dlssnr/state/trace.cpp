#include "state/trace.h"

#include <reshade.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gpu/shader_params.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

struct TraceField {
  std::string name;
  std::string value;
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

Snapshot BuildSnapshot(const Settings& settings,
                       const PassSettings* resolved_passes) {
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
    Snapshot resolved;
    AppendPass(resolved, "resolved[" + std::to_string(i) + "]",
               resolved_passes[i]);
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

}  // namespace

void TraceSettings(const Settings& settings, const PassSettings* resolved) {
  if (!trace::Enabled()) return;
  Snapshot current = BuildSnapshot(settings, resolved);
  EmitSnapshot(g_traced, current, !g_traced_valid);
  g_traced = std::move(current);
  g_traced_valid = true;
}

}  // namespace reshade_dlssnr

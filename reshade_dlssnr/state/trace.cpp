#include "state/trace.h"

#include <string>

#include "pipeline_trace.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "compose/settings.h"

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
  switch (stage) {
    case InjectionStage::PreDLSS: return "PreDLSS";
    case InjectionStage::PostDLSS: return "PostDLSS";
    case InjectionStage::Backbuffer: return "Backbuffer";
    default: return "Unknown";
  }
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

const char* UpsampleModeText(UpsampleMode mode) {
  switch (mode) {
    case UpsampleMode::Easu: return "Easu";
    case UpsampleMode::JointBilateral: return "JointBilateral";
    default: return "JointBilateral";
  }
}
const char* UpsampleKernelText(UpsampleKernel kernel) {
  switch (kernel) {
    case UpsampleKernel::Kernel3x3: return "3x3";
    case UpsampleKernel::Kernel5x5: return "5x5";
    case UpsampleKernel::Kernel7x7: return "7x7";
    default: return "3x3";
  }
}


const char* ChromaModeText(GradeChromaMode mode) {
  switch (mode) {
    case GradeChromaMode::Saturation: return "Saturation";
    case GradeChromaMode::Vibrance: return "Vibrance";
    default: return "Saturation";
  }
}

void Append(Snapshot& snap, const std::string& name, std::string value) {
  snap.push_back(TraceField{std::move(name), std::move(value), std::string(), false});
}

void AppendExposureFilters(Snapshot& snap, const std::string& prefix,
                           const ExposureFilterSettings& filter) {
  Append(snap, prefix + ".importance_mask", BoolText(filter.importance_mask));
  Append(snap, prefix + ".depth", BoolText(filter.depth));
  Append(snap, prefix + ".percentile_range", BoolText(filter.percentile_range));
  Append(snap, prefix + ".depth_near", FloatText(filter.depth_near));
  Append(snap, prefix + ".depth_far", FloatText(filter.depth_far));
  Append(snap, prefix + ".percentile_low", FloatText(filter.percentile_low));
  Append(snap, prefix + ".percentile_high", FloatText(filter.percentile_high));
}

void AppendGrade(Snapshot& snap, const std::string& prefix,
                 const GradeSettings& grade) {
  Append(snap, prefix + ".enabled", BoolText(grade.enabled));
  Append(snap, prefix + ".luma", FloatText(grade.luma));
  Append(snap, prefix + ".chroma", FloatText(grade.chroma));
  Append(snap, prefix + ".luma_mode", LumaModeText(grade.luma_mode));
  Append(snap, prefix + ".chroma_mode", ChromaModeText(grade.chroma_mode));
  Append(snap, prefix + ".black_level", FloatText(grade.black_level));
  Append(snap, prefix + ".white_level", FloatText(grade.white_level));
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
  Append(snap, prefix + ".residual.upsample_sharpness",
         FloatText(pass.residual.upsample_sharpness));
  Append(snap, prefix + ".residual.upsample_kernel",
         UpsampleKernelText(pass.residual.upsample_kernel));
  Append(snap, prefix + ".residual.upsample_mode",
         UpsampleModeText(pass.residual.upsample_mode));
  Append(snap, prefix + ".residual.ratio_eps",
         FloatText(pass.residual.ratio_eps));
  Append(snap, prefix + ".residual.ratio_brighten",
         FloatText(pass.residual.ratio_brighten));
  Append(snap, prefix + ".residual.ratio_darken",
         FloatText(pass.residual.ratio_darken));
  AppendGrade(snap, prefix + ".grade_input", pass.grade_input);
  AppendGrade(snap, prefix + ".grade_output", pass.grade_output);
  Append(snap, prefix + ".strength.luma_strength",
         FloatText(pass.strength.luma_strength));
  Append(snap, prefix + ".strength.chroma_strength",
         FloatText(pass.strength.chroma_strength));
  Append(snap, prefix + ".soft_compression.enabled",
         BoolText(pass.soft_compression.enabled));
  Append(snap, prefix + ".soft_compression.max_brighten",
         FloatText(pass.soft_compression.max_brighten));
  Append(snap, prefix + ".soft_compression.max_darken",
         FloatText(pass.soft_compression.max_darken));
  Append(snap, prefix + ".soft_compression.max_chroma",
         FloatText(pass.soft_compression.max_chroma));
  Append(snap, prefix + ".soft_compression.knee",
         FloatText(pass.soft_compression.knee));
  Append(snap, prefix + ".inherit.working_scale",
         BoolText(pass.inherit_working_scale));
  Append(snap, prefix + ".inherit.intensity", BoolText(pass.inherit_intensity));
  Append(snap, prefix + ".inherit.local_tone",
         BoolText(pass.inherit_local_tone));
  Append(snap, prefix + ".inherit.local_structure",
         BoolText(pass.inherit_local_structure));
  Append(snap, prefix + ".inherit.skin_structure",
         BoolText(pass.inherit_skin_structure));
  Append(snap, prefix + ".inherit.luma_strength",
         BoolText(pass.inherit_luma_strength));
  Append(snap, prefix + ".inherit.chroma_strength",
         BoolText(pass.inherit_chroma_strength));
  Append(snap, prefix + ".inherit.soft_max_brighten",
         BoolText(pass.inherit_soft_max_brighten));
  Append(snap, prefix + ".inherit.soft_max_darken",
         BoolText(pass.inherit_soft_max_darken));
  Append(snap, prefix + ".inherit.soft_max_chroma",
         BoolText(pass.inherit_soft_max_chroma));
  Append(snap, prefix + ".inherit.soft_knee",
         BoolText(pass.inherit_soft_knee));
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
  Append(snap, "hdr_highlight_headroom",
         FloatText(settings.hdr.highlight_headroom));
  const auto& game = settings.hdr.game_exposure;
  Append(snap, "hdr.layer.game.enabled", BoolText(game.enabled));
  Append(snap, "hdr.layer.game.bias_stops", FloatText(game.bias_stops));
  Append(snap, "hdr.layer.game.bounded_tuning", BoolText(game.bounded_tuning));
  Append(snap, "hdr.layer.game.tuning_strength", FloatText(game.tuning_strength));
  Append(snap, "hdr.layer.game.max_tuning_stops", FloatText(game.max_tuning_stops));
  Append(snap, "hdr.layer.game.tuning_midtone", FloatText(game.tuning_midtone));
  Append(snap, "hdr.layer.game.tuning_speed_up", FloatText(game.tuning_speed_up));
  Append(snap, "hdr.layer.game.tuning_speed_down", FloatText(game.tuning_speed_down));
  AppendExposureFilters(snap, "hdr.layer.game.filters", game.filters);

  const auto& adaptive = settings.hdr.adaptive_exposure;
  Append(snap, "hdr.layer.adaptive.enabled", BoolText(adaptive.enabled));
  Append(snap, "hdr.layer.adaptive.midtone", FloatText(adaptive.midtone));
  Append(snap, "hdr.layer.adaptive.speed_up", FloatText(adaptive.speed_up));
  Append(snap, "hdr.layer.adaptive.speed_down", FloatText(adaptive.speed_down));
  Append(snap, "hdr.layer.adaptive.strength_over_game", FloatText(adaptive.strength_over_game));
  Append(snap, "hdr.layer.adaptive.max_correction_stops", FloatText(adaptive.max_correction_stops));
  AppendExposureFilters(snap, "hdr.layer.adaptive.filters", adaptive.filters);

  const auto& local = settings.hdr.local_exposure;
  Append(snap, "hdr.layer.local.enabled", BoolText(local.enabled));
  Append(snap, "hdr.layer.local.strength", FloatText(local.strength));
  Append(snap, "hdr.layer.local.max_brighten", FloatText(local.max_brighten));
  Append(snap, "hdr.layer.local.max_darken", FloatText(local.max_darken));
  Append(snap, "hdr.layer.local.midtone", FloatText(local.midtone));
  AppendExposureFilters(snap, "hdr.layer.local.filters", local.filters);
  Append(snap, "hdr_bt2446_k1", FloatText(settings.hdr.bt2446_k1));
  Append(snap, "hdr_bt2446_k2", FloatText(settings.hdr.bt2446_k2));
  Append(snap, "hdr_bt2446_k3", FloatText(settings.hdr.bt2446_k3));
  Append(snap, "hdr_bt2446_k4", FloatText(settings.hdr.bt2446_k4));
  Append(snap, "hdr_bt2446_crosstalk", FloatText(settings.hdr.bt2446_crosstalk));
  Append(snap, "hdr_gain_min_log2", FloatText(settings.hdr.gain_min_log2));
  Append(snap, "hdr_gain_max_log2", FloatText(settings.hdr.gain_max_log2));
  Append(snap, "importance_mask.enable", BoolText(settings.importance_mask.enable));
  Append(snap, "importance_mask.luma_threshold", FloatText(settings.importance_mask.luma_threshold));
  Append(snap, "importance_mask.luma_relative", FloatText(settings.importance_mask.luma_relative));
  Append(snap, "importance_mask.chroma_threshold", FloatText(settings.importance_mask.chroma_threshold));
  Append(snap, "importance_mask.edit_strength", FloatText(settings.importance_mask.edit_strength));
  Append(snap, "importance_mask.smoothing", FloatText(settings.importance_mask.mask_smoothing));
  Append(snap, "importance_mask.response", FloatText(settings.importance_mask.mask_response));
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

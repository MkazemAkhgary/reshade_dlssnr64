#include "state/trace.h"

#include <string>

#include "pipeline_trace.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "compose/settings.h"
#include "ctl/usage/ui_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/debug_log/view.h"
#include "feature/grade/view.h"
#include "feature/hdr/view.h"
#include "feature/importance_mask/view.h"
#include "feature/pass/settings.h"
#include "feature/preferences/settings.h"
#include "feature/pass/ui.h"
#include "feature/pass/view.h"
#include "hdr/settings.h"
#include "importance_mask/settings.h"

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

// The pass rows, resolved. Takes the store too, because the pass grades and
// the inherit flags live in their own rows and their own sections -- they are
// not part of PassSnapshot -- and a trace of the pass that omitted them would
// be a trace of three quarters of one.
void AppendPass(ctl::ValueStore& store, const ctl::InstanceKey& key,
                Snapshot& snap, const std::string& prefix,
                const feature::pass::PassSnapshot& pass) {
  Append(snap, prefix + ".preset", std::to_string(pass.preset));
  Append(snap, prefix + ".local_tone", FloatText(pass.local_tone));
  Append(snap, prefix + ".ui_correction", BoolText(pass.ui_correction));
  Append(snap, prefix + ".local_structure", FloatText(pass.local_structure));
  Append(snap, prefix + ".auto_mask", BoolText(pass.auto_mask));
  Append(snap, prefix + ".skin_structure", FloatText(pass.skin_structure));
  Append(snap, prefix + ".intensity", FloatText(pass.intensity));
  Append(snap, prefix + ".working_scale", FloatText(pass.working_scale));
  Append(snap, prefix + ".target", std::to_string(pass.target));
  Append(snap, prefix + ".custom_name", pass.custom_name);
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
  AppendGrade(snap, prefix + ".grade_input",
              feature::grade::Read(store, key, feature::pass::kGradeInputSection,
                                   feature::pass::kGradeInputPrefix));
  AppendGrade(snap, prefix + ".grade_output",
              feature::grade::Read(store, key, feature::pass::kGradeOutputSection,
                                   feature::pass::kGradeOutputPrefix));
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
  Append(snap, prefix + ".rcas_enabled", BoolText(pass.rcas_enabled));
  Append(snap, prefix + ".rcas_strength", FloatText(pass.rcas_strength));

}

Snapshot BuildSnapshot(const Settings& settings) {
  Snapshot snap;
  // HDR and the importance mask are read from the store, not from `settings`.
  // They are single-instance sections the UI edits through the store, so a
  // copy carried in Settings would be frozen at load time and the dump would
  // report values the user has already changed. Only the raw-vs-resolved pass
  // comparison and the backbuffer grade still come from Settings.
  const ctl::InstanceKey shared_key = ctl::InstanceKey::FromInt(0);
  ctl::ValueStore& store = ctl::Store();
  const HdrSettings hdr = feature::hdr::Read(store, shared_key);
  const ImportanceMaskSettings mask =
      feature::importance_mask::Read(store, shared_key);

  Append(snap, "enabled", BoolText(settings.enabled));
  Append(snap, "debug_view", std::to_string(
      feature::debug_log::Read(store, shared_key)));
  Append(snap, "show_tooltips", BoolText(store.Read(
      feature::preferences::kSection, shared_key,
      feature::preferences::kShowTooltips).as_bool));
  Append(snap, "require_dlss", BoolText(store.Read(
      feature::preferences::kSection, shared_key,
      feature::preferences::kRequireDlss).as_bool));
  Append(snap, "clone_resources", BoolText(store.Read(
      feature::preferences::kSection, shared_key,
      feature::preferences::kCloneResources).as_bool));
  Append(snap, "active_pass", std::to_string(settings.active_pass));
  Append(snap, "hdr_enable", BoolText(hdr.enable));
  Append(snap, "hdr_format", HdrFormatText(hdr.format));
  Append(snap, "hdr_measure_interval",
         std::to_string(hdr.measure_interval));
  Append(snap, "hdr_highlight_headroom",
         FloatText(hdr.highlight_headroom));
  const auto& game = hdr.game_exposure;
  Append(snap, "hdr.layer.game.enabled", BoolText(game.enabled));
  Append(snap, "hdr.layer.game.bias_stops", FloatText(game.bias_stops));
  Append(snap, "hdr.layer.game.bounded_tuning", BoolText(game.bounded_tuning));
  Append(snap, "hdr.layer.game.tuning_strength", FloatText(game.tuning_strength));
  Append(snap, "hdr.layer.game.max_tuning_stops", FloatText(game.max_tuning_stops));
  Append(snap, "hdr.layer.game.tuning_midtone", FloatText(game.tuning_midtone));
  Append(snap, "hdr.layer.game.tuning_speed_up", FloatText(game.tuning_speed_up));
  Append(snap, "hdr.layer.game.tuning_speed_down", FloatText(game.tuning_speed_down));
  AppendExposureFilters(snap, "hdr.layer.game.filters", game.filters);

  const auto& adaptive = hdr.adaptive_exposure;
  Append(snap, "hdr.layer.adaptive.enabled", BoolText(adaptive.enabled));
  Append(snap, "hdr.layer.adaptive.midtone", FloatText(adaptive.midtone));
  Append(snap, "hdr.layer.adaptive.speed_up", FloatText(adaptive.speed_up));
  Append(snap, "hdr.layer.adaptive.speed_down", FloatText(adaptive.speed_down));
  Append(snap, "hdr.layer.adaptive.strength_over_game", FloatText(adaptive.strength_over_game));
  Append(snap, "hdr.layer.adaptive.max_correction_stops", FloatText(adaptive.max_correction_stops));
  AppendExposureFilters(snap, "hdr.layer.adaptive.filters", adaptive.filters);

  const auto& local = hdr.local_exposure;
  Append(snap, "hdr.layer.local.enabled", BoolText(local.enabled));
  Append(snap, "hdr.layer.local.strength", FloatText(local.strength));
  Append(snap, "hdr.layer.local.max_brighten", FloatText(local.max_brighten));
  Append(snap, "hdr.layer.local.max_darken", FloatText(local.max_darken));
  Append(snap, "hdr.layer.local.midtone", FloatText(local.midtone));
  AppendExposureFilters(snap, "hdr.layer.local.filters", local.filters);
  Append(snap, "hdr_bt2446_k1", FloatText(hdr.bt2446_k1));
  Append(snap, "hdr_bt2446_k2", FloatText(hdr.bt2446_k2));
  Append(snap, "hdr_bt2446_k3", FloatText(hdr.bt2446_k3));
  Append(snap, "hdr_bt2446_k4", FloatText(hdr.bt2446_k4));
  Append(snap, "hdr_bt2446_crosstalk", FloatText(hdr.bt2446_crosstalk));
  Append(snap, "hdr_gain_min_log2", FloatText(hdr.gain_min_log2));
  Append(snap, "hdr_gain_max_log2", FloatText(hdr.gain_max_log2));
  Append(snap, "importance_mask.enable", BoolText(mask.enable));
  Append(snap, "importance_mask.luma_threshold", FloatText(mask.luma_threshold));
  Append(snap, "importance_mask.luma_relative", FloatText(mask.luma_relative));
  Append(snap, "importance_mask.chroma_threshold", FloatText(mask.chroma_threshold));
  Append(snap, "importance_mask.edit_strength", FloatText(mask.edit_strength));
  Append(snap, "importance_mask.smoothing", FloatText(mask.mask_smoothing));
  Append(snap, "importance_mask.response", FloatText(mask.mask_response));
  AppendGrade(snap, "grade_backbuffer",
              feature::grade::ReadBackbuffer(store));

  // Pass rows. Dumped once per pass, in tab order, from the store's resolved
  // value -- the same number the render path runs at. The raw value is
  // available too, as the pass's own un-inherited override, and the trace
  // prints it beside the resolved one when they differ, which is exactly the
  // information this dump exists to show: which settings are inherited.
  for (size_t position = 0; position < settings.pass_order.size(); ++position) {
    const feature::pass::PassId id = settings.pass_order[position];
    const ctl::InstanceKey key = feature::pass::PassKey(id);
    const std::string prefix =
        "pass[" + std::to_string(position) + "]";
    const feature::pass::PassSnapshot resolved =
        feature::pass::Read(store, key);
    AppendPass(store, key, snap, prefix, resolved);
    Append(snap, prefix + ".enabled",
           BoolText(store.Read(feature::pass::kSection, key,
                               feature::pass::kPassEnabled)
                        .as_bool));
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

void TraceSettings(const Settings& settings) {
  if (!trace::Enabled()) return;
  Snapshot current = BuildSnapshot(settings);
  EmitSnapshot(g_traced, current, !g_traced_valid);
  g_traced = std::move(current);
  g_traced_valid = true;
}

}  // namespace reshade_dlssnr

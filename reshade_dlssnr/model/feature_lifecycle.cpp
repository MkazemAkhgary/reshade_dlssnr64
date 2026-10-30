#include "model/feature_lifecycle.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <reshade.hpp>

#include "model/ngx_model_params.h"

namespace reshade_dlssnr {

namespace {

constexpr float kModelSettingsEpsilon = 1.0e-5f;

const char* StyleName(unsigned int style) {
  switch (style) {
    case 0: return "A";
    case 1: return "B";
    case 2: return "C";
    default: return "?";
  }
}

const char* TargetName(int target) {
  switch (target) {
    case 0: return "Pre-DLSS";
    case 1: return "Post-DLSS";
    case 2: return "Backbuffer";
    default: return "?";
  }
}

bool NearlyEqual(float a, float b) {
  return std::fabs(a - b) <= kModelSettingsEpsilon;
}

}  // namespace

void FeatureLifecycle::ObserveModelSettings(
    const feature::pass::PassSnapshot& pass) {
  ModelSettingsState current = {};
  current.target = std::clamp(pass.target, 0, 2);
  current.style = static_cast<unsigned int>(std::clamp(pass.preset, 0, 2));
  current.render_preset =
      static_cast<unsigned int>(std::clamp(pass.render_preset, 1, 3));
  current.working_scale = pass.working_scale;
  current.working_scale_x2 = pass.working_scale_x2;
  current.local_tone = pass.local_tone;
  current.local_structure = pass.local_structure;
  current.skin_structure = pass.auto_mask ? pass.skin_structure : -1.0f;
  current.auto_mask = pass.auto_mask;
  current.ui_correction = pass.ui_correction;

  if (model_settings_valid_) {
    const bool style_changed = current.style != model_settings_.style;
    const bool target_changed = current.target != model_settings_.target;
    const bool settings_changed =
        style_changed || target_changed ||
        current.render_preset != model_settings_.render_preset ||
        !NearlyEqual(current.working_scale, model_settings_.working_scale) ||
        current.working_scale_x2 != model_settings_.working_scale_x2 ||
        !NearlyEqual(current.local_tone, model_settings_.local_tone) ||
        !NearlyEqual(current.local_structure, model_settings_.local_structure) ||
        !NearlyEqual(current.skin_structure, model_settings_.skin_structure) ||
        current.auto_mask != model_settings_.auto_mask ||
        current.ui_correction != model_settings_.ui_correction;

    if (settings_changed) {
      // DLSSNR keeps its own temporal state inside the NGX feature. Updating
      // Style (or changing the stage/input contract) without Reset leaves
      // that state conditioned by the previous configuration. Keep the
      // reset pending until an EvaluateFeature call actually succeeds.
      reset_pending_ = true;

      std::string message = "DLSSNR model settings changed:";
      if (style_changed) {
        message += " Style ";
        message += StyleName(model_settings_.style);
        message += " -> ";
        message += StyleName(current.style);
      }
      if (target_changed) {
        message += " Target ";
        message += TargetName(model_settings_.target);
        message += " -> ";
        message += TargetName(current.target);
      }
      if (current.render_preset != model_settings_.render_preset) {
        message += " RenderPreset ";
        message += std::to_string(model_settings_.render_preset);
        message += " -> ";
        message += std::to_string(current.render_preset);
      }
      if (!style_changed && !target_changed &&
          current.render_preset == model_settings_.render_preset) {
        message += " model input controls";
      }
      message +=
          " (NGX Reset will be asserted on the next successful evaluation)";
      reshade::log::message(reshade::log::level::info, message.c_str());
    }
  }

  model_settings_ = current;
  model_settings_valid_ = true;
}

bool FeatureLifecycle::EnsureFeature(
    NgxRuntime& runtime,
    const feature::pass::PassSnapshot& pass,
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  DLSSNR_PROFILE_CORE_L2("FeatureLifecycle::EnsureFeature");
  // Read the current pass snapshot every time. Model tuning is written to the
  // shared NGX parameter block per evaluation, but NGX also retains temporal
  // state internally; changes to model identity or its input contract need a
  // one-shot Reset without rebuilding the feature.
  ObserveModelSettings(pass);

  // Only a size change requires a feature rebuild. Rebuilding for tuning
  // changes was causing unnecessary feature churn during slider edits.
  if (feature_handle_ != nullptr &&
      built_width_ == width &&
      built_height_ == height) {
    return true;
  }

  if (feature_handle_ != nullptr) {
    runtime.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
    reset_pending_ = true;
  }

  just_rebuilt_ = true;

  NgxParameterBlock& parameter_block = runtime.Parameters();
  NVSDK_NGX_Parameter* params = parameter_block.Get();

  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return false;
  }

  // No Reset(): this block is shared with Streamline's sl.common for the life of
  // the process. Every key we read is one we write below.
  parameter_block.SetUInt("DLSSNR.Width", width);
  parameter_block.SetUInt("DLSSNR.Height", height);
  parameter_block.SetUInt("DLSSNR.InputWidth", width);
  parameter_block.SetUInt("DLSSNR.InputHeight", height);
  parameter_block.SetUInt("DLSSNR.OutputWidth", width);
  parameter_block.SetUInt("DLSSNR.OutputHeight", height);
  parameter_block.SetUInt(NVSDK_NGX_Parameter_PerfQualityValue, 2u);
  parameter_block.SetUInt("CreationNodeMask", 1u);
  parameter_block.SetUInt("VisibilityNodeMask", 1u);
  ApplyModelTuningParams(parameter_block, pass);

  // Logged before the call, not after: a crash inside the model's
  // CreateFeature is otherwise indistinguishable from one just after it,
  // because the only line either path produced was the success message.
  {
    std::string msg = "DLSSNR EnsureFeature: creating feature at ";
    msg += std::to_string(width) + "x" + std::to_string(height);
    reshade::log::message(reshade::log::level::info, msg.c_str());
  }

  const NVSDK_NGX_Result result = runtime.CreateFeature(
      command_list,
      static_cast<NVSDK_NGX_Feature>(18),
      params,
      &feature_handle_);

  if (result != NVSDK_NGX_Result_Success || feature_handle_ == nullptr) {
    feature_handle_ = nullptr;
    reason_ = "DLSSNR CreateFeature failed";
    reshade::log::message(reshade::log::level::error, reason_);
    return false;
  }

  built_width_ = width;
  built_height_ = height;

  std::string msg = "Direct DLSSNR feature created at ";
  msg += std::to_string(width) + "x" + std::to_string(height);
  reshade::log::message(reshade::log::level::info, msg.c_str());

  return true;
}

void FeatureLifecycle::ReleaseFeature(NgxRuntime& runtime) {
  DLSSNR_PROFILE_RESOURCE_L2("FeatureLifecycle::ReleaseFeature");
  reset_pending_ = true;
  if (feature_handle_ == nullptr) return;

  runtime.ReleaseFeature(feature_handle_);
  feature_handle_ = nullptr;
}

void FeatureLifecycle::Shutdown(NgxRuntime& runtime) {
  DLSSNR_PROFILE_RESOURCE_L2("FeatureLifecycle::Shutdown");
  ReleaseFeature(runtime);

  built_width_ = 0;
  built_height_ = 0;
  reset_pending_ = true;
  just_rebuilt_ = false;
  model_settings_ = {};
  model_settings_valid_ = false;
}

}  // namespace reshade_dlssnr

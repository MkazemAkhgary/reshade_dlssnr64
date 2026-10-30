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

bool FeatureLifecycle::ObserveModelSettings(
    const feature::pass::PassSnapshot& pass) {
  bool render_preset_changed = false;
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
    render_preset_changed =
        current.render_preset != model_settings_.render_preset;
    const bool reset_required =
        style_changed || target_changed ||
        !NearlyEqual(current.working_scale, model_settings_.working_scale) ||
        current.working_scale_x2 != model_settings_.working_scale_x2 ||
        !NearlyEqual(current.local_tone, model_settings_.local_tone) ||
        !NearlyEqual(current.local_structure, model_settings_.local_structure) ||
        !NearlyEqual(current.skin_structure, model_settings_.skin_structure) ||
        current.auto_mask != model_settings_.auto_mask ||
        current.ui_correction != model_settings_.ui_correction;
    const bool settings_changed = reset_required || render_preset_changed;

    if (reset_required) {
      // NGX keeps temporal state inside the feature. Changes to live model
      // controls require Reset; Render Preset is different because NGX latches
      // it at creation, and a rebuilt feature starts with fresh history.
      reset_pending_ = true;
    }

    if (settings_changed) {
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
      if (render_preset_changed) {
        message += " RenderPreset ";
        message += std::to_string(model_settings_.render_preset);
        message += " -> ";
        message += std::to_string(current.render_preset);
      }
      if (!style_changed && !target_changed && !render_preset_changed) {
        message += " model input controls";
      }
      if (reset_required) {
        message +=
            " (NGX Reset will be asserted on the next successful evaluation)";
      } else {
        message += " (NGX feature rebuild required)";
      }
      reshade::log::message(reshade::log::level::info, message.c_str());
    }
  }

  model_settings_ = current;
  model_settings_valid_ = true;
  return render_preset_changed;
}

bool FeatureLifecycle::EnsureFeature(
    NgxRuntime& runtime,
    const feature::pass::PassSnapshot& pass,
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  DLSSNR_PROFILE_CORE_L2("FeatureLifecycle::EnsureFeature");
  // Read the current pass snapshot every time. Most model controls are live
  // evaluation parameters and only need the one-shot temporal Reset. NGX
  // latches DLSSNR.Hint.Render.Preset at CreateFeature, however, so changing
  // that specific value must release and recreate the feature.
  const bool render_preset_changed = ObserveModelSettings(pass);

  if (feature_handle_ != nullptr && !render_preset_changed &&
      built_width_ == width && built_height_ == height) {
    return true;
  }

  if (feature_handle_ != nullptr) {
    // An earlier EvaluateFeature may still have GPU work in flight. Keep the
    // old handle alive until the controller's queue marker completes.
    RetireFeature(!render_preset_changed);
    if (render_preset_changed) {
      reshade::log::message(
          reshade::log::level::info,
          "DLSSNR Render Preset changed; rebuilding NGX feature");
    }
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

void FeatureLifecycle::RetireFeature(bool request_reset) {
  DLSSNR_PROFILE_RESOURCE_L2("FeatureLifecycle::RetireFeature");
  if (request_reset) reset_pending_ = true;
  if (feature_handle_ == nullptr) return;

  retired_features_.push_back({feature_handle_, 0});
  feature_handle_ = nullptr;
  reshade::log::message(
      reshade::log::level::info,
      "DLSSNR: queued previous NGX feature for fence-delayed release");
}

bool FeatureLifecycle::HasUnfencedRetiredFeatures() const {
  for (const RetiredFeature& retired : retired_features_) {
    if (retired.handle != nullptr && retired.fence_value == 0) return true;
  }
  return false;
}

void FeatureLifecycle::TagRetiredFeatures(uint64_t fence_value) {
  if (fence_value == 0) return;
  for (RetiredFeature& retired : retired_features_) {
    if (retired.handle != nullptr && retired.fence_value == 0) {
      retired.fence_value = fence_value;
    }
  }
}

void FeatureLifecycle::ReleaseRetiredFeaturesCompleted(
    NgxRuntime& runtime, uint64_t completed_fence_value) {
  for (auto it = retired_features_.begin(); it != retired_features_.end();) {
    if (it->handle != nullptr && it->fence_value != 0 &&
        it->fence_value <= completed_fence_value) {
      const NVSDK_NGX_Result result = runtime.ReleaseFeature(it->handle);
      if (result != NVSDK_NGX_Result_Success) {
        std::string message = "DLSSNR: deferred ReleaseFeature failed: ";
        message += NgxResultName(result);
        reshade::log::message(reshade::log::level::warning, message.c_str());
      }
      it = retired_features_.erase(it);
    } else {
      ++it;
    }
  }
}

void FeatureLifecycle::Shutdown(NgxRuntime& runtime) {
  DLSSNR_PROFILE_RESOURCE_L2("FeatureLifecycle::Shutdown");
  // Shutdown is only called after the controller has drained the device or
  // observed the retirement fence, so all remaining handles are now safe.
  if (feature_handle_ != nullptr) {
    runtime.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
  }
  for (const RetiredFeature& retired : retired_features_) {
    if (retired.handle != nullptr) runtime.ReleaseFeature(retired.handle);
  }
  retired_features_.clear();

  built_width_ = 0;
  built_height_ = 0;
  reset_pending_ = true;
  just_rebuilt_ = false;
  model_settings_ = {};
  model_settings_valid_ = false;
}

}  // namespace reshade_dlssnr

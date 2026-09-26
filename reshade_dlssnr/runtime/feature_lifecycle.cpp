#include "feature_lifecycle.h"

#include <algorithm>
#include <string>

#include <reshade.hpp>

#include "ngx_model_params.h"

namespace reshade_dlssnr {

bool FeatureLifecycle::EnsureFeature(
    NgxRuntime& runtime,
    const PassSettings& pass,
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  // Only a size change requires a rebuild. Tuning values (preset, intensity,
  // structure strengths, etc.) are re-applied at every evaluation via
  // FillEvaluateParams -> ApplyModelTuningParams, so they never need to
  // trigger a feature rebuild. Rebuilding on tuning was causing a rebuild on
  // every slider drag and making the UI sluggish.
  if (feature_handle_ != nullptr &&
      built_width_ == width &&
      built_height_ == height) {
    return true;
  }

  if (feature_handle_ != nullptr) {
    runtime.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
    first_evaluate_ = true;
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
  if (feature_handle_ == nullptr) return;

  runtime.ReleaseFeature(feature_handle_);
  feature_handle_ = nullptr;
}

void FeatureLifecycle::Shutdown(NgxRuntime& runtime) {
  ReleaseFeature(runtime);

  built_width_ = 0;
  built_height_ = 0;
  first_evaluate_ = true;
  just_rebuilt_ = false;
}

}  // namespace reshade_dlssnr

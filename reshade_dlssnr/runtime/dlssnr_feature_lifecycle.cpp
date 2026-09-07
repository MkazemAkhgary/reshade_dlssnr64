#include "dlssnr_feature_lifecycle.h"

#include <algorithm>
#include <string>

#include <reshade.hpp>

namespace reshade_dlssnr {

bool DlssNrFeatureLifecycle::EnsureFeature(
    DlssNrRuntime& runtime,
    const PassSettings& pass,
    ID3D12GraphicsCommandList* command_list,
    unsigned int width,
    unsigned int height) {
  const unsigned int model_value =
      static_cast<unsigned int>(std::clamp(pass.preset, 0, 2));

  const bool tuning_changed =
      built_preset_ != model_value ||
      built_intensity_ != pass.intensity ||
      built_local_structure_ != pass.local_structure ||
      built_local_tone_ != pass.local_tone ||
      built_skin_structure_ != pass.skin_structure ||
      built_auto_mask_ != pass.auto_mask ||
      built_ui_correction_ != pass.ui_correction;

  if (feature_handle_ != nullptr &&
      built_width_ == width &&
      built_height_ == height &&
      !tuning_changed) {
    return true;
  }

  if (feature_handle_ != nullptr) {
    runtime.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
    first_evaluate_ = true;
  }

  NgxParameterBlock& parameter_block = const_cast<DlssNrRuntime&>(runtime).Parameters();
  NVSDK_NGX_Parameter* params = parameter_block.Get();

  if (params == nullptr) {
    reason_ = "DLSSNR parameter block unavailable";
    return false;
  }

  params->Reset();

  parameter_block.SetUInt("DLSSNR.Enabled", 1u);
  parameter_block.SetUInt("DLSSNR.Width", width);
  parameter_block.SetUInt("DLSSNR.Height", height);
  parameter_block.SetUInt("DLSSNR.InputWidth", width);
  parameter_block.SetUInt("DLSSNR.InputHeight", height);
  parameter_block.SetUInt("DLSSNR.OutputWidth", width);
  parameter_block.SetUInt("DLSSNR.OutputHeight", height);
  parameter_block.SetUInt("DLSSNR.Upscaling", 1u);
  parameter_block.SetFloat("DLSSNR.ScalingRatio", 1.0f);
  parameter_block.SetFloat("DLSSNR.Scale", 1.0f);
  parameter_block.SetUInt("DLSSNR.Hint.Render.Preset", model_value + 1u);
  parameter_block.SetUInt(NVSDK_NGX_Parameter_PerfQualityValue, 2u);
  parameter_block.SetFloat("DLSSNR.Intensity", pass.intensity);
  parameter_block.SetUInt("DLSSNR.Style", model_value);
  parameter_block.SetFloat(
      "DLSSNR.LocalStructureStrength", pass.local_structure);
  parameter_block.SetFloat("DLSSNR.LocalToneStrength", pass.local_tone);
  parameter_block.SetFloat(
      "DLSSNR.SkinStructureStrength",
      pass.auto_mask ? pass.skin_structure : -1.0f);
  parameter_block.SetUInt(
      "DLSSNR.UseAutoMask", pass.auto_mask ? 1u : 0u);
  parameter_block.SetUInt(
      "DLSSNR.UICorrection", pass.ui_correction ? 1u : 0u);
  parameter_block.SetUInt("CreationNodeMask", 1u);
  parameter_block.SetUInt("VisibilityNodeMask", 1u);

  const NVSDK_NGX_Result result = const_cast<DlssNrRuntime&>(runtime).CreateFeature(
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
  built_working_scale_ = pass.working_scale;
  built_preset_ = model_value;
  built_intensity_ = pass.intensity;
  built_local_structure_ = pass.local_structure;
  built_local_tone_ = pass.local_tone;
  built_skin_structure_ = pass.skin_structure;
  built_auto_mask_ = pass.auto_mask;
  built_ui_correction_ = pass.ui_correction;

  std::string msg = "Direct DLSSNR feature created at ";
  msg += std::to_string(width) + "x" + std::to_string(height);
  reshade::log::message(reshade::log::level::info, msg.c_str());

  return true;
}

void DlssNrFeatureLifecycle::ReleaseFeature(DlssNrRuntime& runtime) {
  if (feature_handle_ != nullptr) {
    runtime.ReleaseFeature(feature_handle_);
    feature_handle_ = nullptr;
  }
}

void DlssNrFeatureLifecycle::Shutdown(DlssNrRuntime& runtime) {
  ReleaseFeature(runtime);

  built_width_ = 0;
  built_height_ = 0;
  built_working_scale_ = -1.0f;
  built_preset_ = 0xFFFFFFFFu;
  built_intensity_ = -1.0f;
  built_local_structure_ = -1.0f;
  built_local_tone_ = -1.0f;
  built_skin_structure_ = -1.0f;
  built_auto_mask_ = false;
  built_ui_correction_ = false;
  first_evaluate_ = true;
}

}  // namespace reshade_dlssnr

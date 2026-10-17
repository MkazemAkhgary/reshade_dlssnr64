#include "grade/grade.h"

#include <algorithm>

#include "hdr/settings.h"

namespace reshade_dlssnr::model_pipeline {

void SetGradeConstants(ComputeConstants& constants, const GradeSettings& grade) {
  constants.grade.Enabled = grade.enabled ? 1.0f : 0.0f;
  constants.grade.Luma = grade.luma;
  constants.grade.Chroma = grade.chroma;
  constants.grade.LumaMode = static_cast<float>(static_cast<int>(grade.luma_mode));
  constants.grade.ChromaMode = static_cast<float>(static_cast<int>(grade.chroma_mode));
  constants.grade.BlackLevel = grade.black_level;
  constants.grade.WhiteLevel = grade.white_level;
  constants.grade.Contrast = grade.contrast;
  constants.grade.Shadows = grade.shadows;
  constants.grade.Highlights = grade.highlights;
  constants.grade.Format = 0.0f;
  constants.grade.RestoreExposure = -1.0f;
  constants.hdr.LocalExposureParams.w = 1.0f;
}

bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                int format_code,
                const GradeSettings& grade,
                ID3D12Resource* local_carrier,
                ID3D12Resource* global_carrier,
                ID3D12Resource* global_scale,
                float restore_mode,
                float restore_strength) {
  const bool restore_exposure =
      local_carrier != nullptr && global_carrier != nullptr;
  if (!grade.enabled && !restore_exposure) return true;

  if (codec == nullptr || command_list == nullptr || target == nullptr ||
      scratch == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  // This path also runs when only exposure restoration is enabled.
  SetGradeConstants(constants, grade);
  constants.grade.Format = static_cast<float>(format_code);
  constants.grade.RestoreExposure =
      restore_exposure ? std::clamp(restore_mode, 0.0f, 1.0f) : -1.0f;
  constants.hdr.LocalExposureParams.w =
      restore_exposure ? std::clamp(restore_strength, 0.0f, 1.0f) : 1.0f;

  const ResourceTransition before_copy[] = {
      {target, D3D12_RESOURCE_STATE_COPY_SOURCE},
      {scratch, D3D12_RESOURCE_STATE_COPY_DEST},
  };
  state.TransitionBatch(command_list, before_copy, 2);
  command_list->CopyResource(scratch, target);

  ResourceTransition after_copy[] = {
      {scratch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
      {restore_exposure ? local_carrier : nullptr,
       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {restore_exposure ? global_carrier : nullptr,
       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {restore_exposure && global_scale != nullptr ? global_scale : nullptr,
       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
  };
  state.TransitionBatch(command_list, after_copy, 5);

  ID3D12Resource* local_srv = restore_exposure ? local_carrier : scratch;
  ID3D12Resource* global_srv = restore_exposure ? global_carrier : scratch;
  ID3D12Resource* scale_srv =
      restore_exposure && global_scale != nullptr ? global_scale : scratch;
  ID3D12Resource* srvs[kSrvCount] = {scratch, local_srv, global_srv, scale_srv};
  ID3D12Resource* uavs[kUavCount] = {target};

  return codec->Dispatch("ApplyGrade", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

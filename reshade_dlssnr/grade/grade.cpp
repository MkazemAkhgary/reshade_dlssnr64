#include "grade/grade.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cstdint>

#define A_CPU 1
#include "../third_party/FidelityFX-CAS/ffx-cas/ffx_a.h"
#include "../third_party/FidelityFX-CAS/ffx-cas/ffx_cas.h"
#undef A_CPU

#include "hdr/settings.h"

namespace reshade_dlssnr::model_pipeline {

void SetGradeConstants(ComputeConstants& constants, const GradeSettings& grade) {
  DLSSNR_PROFILE_RENDER_L3("SetGradeConstants");
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
  constants.grade.EdgeSofteningEnabled =
      grade.enabled && grade.edge_softening_enabled ? 1.0f : 0.0f;
  constants.grade.EdgeSoftening = std::clamp(grade.edge_softening, 0.0f, 1.0f);
  constants.grade.CasEnabled =
      grade.enabled && grade.cas_enabled ? 1.0f : 0.0f;
  constants.grade.CasSharpness = std::clamp(grade.cas_sharpness, 0.0f, 1.0f);
  constants.grade.CasConst0 = {};
  constants.grade.CasConst1 = {};
  constants.hdr.LocalExposureParams.w = 1.0f;
}

bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* source,
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
  DLSSNR_PROFILE_RENDER_L2("ApplyGrade");
  const bool restore_exposure =
      local_carrier != nullptr && global_carrier != nullptr;
  if (!grade.enabled && !restore_exposure) return true;

  if (codec == nullptr || command_list == nullptr || source == nullptr ||
      target == nullptr || scratch == nullptr) {
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

  // FidelityFX CAS is defined for linear [0, 1] color. Compute its dimension-
  // dependent constants on CPU only when the option is active and the target
  // is SDR; the shader explicitly skips CAS for PQ/scRGB.
  if (grade.enabled && grade.cas_enabled && format_code == 0) {
    varAU4(const0) = {};
    varAU4(const1) = {};
    CasSetup(const0, const1, std::clamp(grade.cas_sharpness, 0.0f, 1.0f),
             static_cast<float>(width), static_cast<float>(height),
             static_cast<float>(width), static_cast<float>(height));
    constants.grade.CasConst0 = {const0[0], const0[1], const0[2], const0[3]};
    constants.grade.CasConst1 = {const1[0], const1[1], const1[2], const1[3]};
  }

  const ResourceTransition before_copy[] = {
      {source, D3D12_RESOURCE_STATE_COPY_SOURCE},
      {scratch, D3D12_RESOURCE_STATE_COPY_DEST},
  };
  state.TransitionBatch(command_list, before_copy, 2);
  command_list->CopyResource(scratch, source);

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

  if (!codec->Dispatch("ApplyGrade", command_list, constants, srvs, uavs))
    return false;

  if (!grade.enabled || !grade.cas_enabled || format_code != 0) return true;

  const ResourceTransition cas_before[] = {
      {target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
  };
  state.TransitionBatch(command_list, cas_before, 2);
  ID3D12Resource* cas_srvs[kSrvCount] = {target, target, target, target};
  ID3D12Resource* cas_uavs[kUavCount] = {scratch};
  if (!codec->Dispatch("ApplyCas", command_list, constants, cas_srvs, cas_uavs))
    return false;

  const ResourceTransition cas_copy_before[] = {
      {scratch, D3D12_RESOURCE_STATE_COPY_SOURCE},
      {target, D3D12_RESOURCE_STATE_COPY_DEST},
  };
  state.TransitionBatch(command_list, cas_copy_before, 2);
  command_list->CopyResource(target, scratch);
  const ResourceTransition cas_copy_after[] = {
      {scratch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
  };
  state.TransitionBatch(command_list, cas_copy_after, 2);
  return true;
}

}  // namespace reshade_dlssnr::model_pipeline

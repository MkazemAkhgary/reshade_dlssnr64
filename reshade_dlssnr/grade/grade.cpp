#include "grade/grade.h"

#include "hdr/settings.h"

namespace reshade_dlssnr::model_pipeline {

bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                int format_code,
                const GradeSettings& grade,
                ID3D12Resource* exposure_map) {
  const bool unexpose = exposure_map != nullptr;
  if (!grade.enabled && !unexpose) return true;

  if (codec == nullptr || command_list == nullptr || target == nullptr ||
      scratch == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  // Set from the flag, not 1.0f: this path also runs for unexpose-only.
  constants.grade.Enabled = grade.enabled ? 1.0f : 0.0f;
  constants.grade.Luma = grade.luma;
  constants.grade.Chroma = grade.chroma;
  constants.grade.LumaMode =
      static_cast<float>(static_cast<int>(grade.luma_mode));
  constants.grade.ChromaMode =
      static_cast<float>(static_cast<int>(grade.chroma_mode));
  constants.grade.BlackLevel = grade.black_level;
  constants.grade.WhiteLevel = grade.white_level;
  constants.grade.Contrast = grade.contrast;
  constants.grade.Shadows = grade.shadows;
  constants.grade.Highlights = grade.highlights;
  constants.grade.Format =
      static_cast<float>(format_code);
  constants.grade.Unexpose = unexpose ? 1.0f : 0.0f;

  state.Transition(command_list, target, D3D12_RESOURCE_STATE_COPY_SOURCE);
  state.Transition(command_list, scratch, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(scratch, target);
  state.Transition(command_list, scratch,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (unexpose) {
    state.Transition(command_list, exposure_map,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // Every slot must hold a valid resource; the source stands in for the
  // unused exposure slot when the flag is off.
  ID3D12Resource* exposure_srv = unexpose ? exposure_map : scratch;

  ID3D12Resource* srvs[kSrvCount] = {scratch, exposure_srv, scratch, scratch};
  ID3D12Resource* uavs[kUavCount] = {target};

  return codec->Dispatch("ApplyGrade", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

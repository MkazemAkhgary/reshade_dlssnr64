#include "runtime/grade.h"

#include "settings/hdr.h"
#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                HdrFormat format,
                const GradeSettings& grade) {
  if (!grade.enabled) return true;

  if (codec == nullptr || command_list == nullptr || target == nullptr ||
      scratch == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.params[kGradeSlotBase + 0] = 1.0f;
  constants.params[kGradeSlotBase + 1] = grade.luma;
  constants.params[kGradeSlotBase + 2] = grade.chroma;
  constants.params[kGradeSlotBase + 3] =
      static_cast<float>(static_cast<int>(grade.luma_mode));
  constants.params[kGradeSlotBase + 4] =
      static_cast<float>(static_cast<int>(grade.chroma_mode));
  constants.params[kGradeSlotBase + 5] = grade.black_point;
  constants.params[kGradeSlotBase + 6] = grade.white_point;
  constants.params[kGradeSlotBase + 7] = grade.contrast;
  constants.params[kGradeSlotBase + 8] = grade.shadows;
  constants.params[kGradeSlotBase + 9] = grade.highlights;
  constants.params[kGradeSlotBase + 10] =
      static_cast<float>(HdrFormatCode(format));

  state.Transition(command_list, target, D3D12_RESOURCE_STATE_COPY_SOURCE);
  state.Transition(command_list, scratch, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(scratch, target);
  state.Transition(command_list, scratch,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {scratch, scratch, scratch};
  ID3D12Resource* uavs[kUavCount] = {target};

  return codec->Dispatch("ApplyGrade", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

#include "dlssnr_stages_working_scale.h"

#include <cstring>

#include "shader_params.h"

namespace reshade_dlssnr::dlssnr_stages_working_scale {

bool PrepareInput(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Resource* full_input,
    ID3D12Resource** out_model_color,
    ID3D12Resource** out_model_output,
    unsigned int* out_eval_width,
    unsigned int* out_eval_height,
    unsigned int full_width,
    unsigned int full_height,
    unsigned int work_width,
    unsigned int work_height,
    bool use_working_scale) {
  if (out_model_color == nullptr || out_model_output == nullptr) return false;

  *out_model_color = full_input;
  *out_model_output = resources.GetOutput();
  *out_eval_width = full_width;
  *out_eval_height = full_height;

  if (!use_working_scale) return true;
  if (codec == nullptr || full_input == nullptr) return false;

  DlssNrConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;

  ID3D12Resource* srvs[kDlssNrSrvCount] = {full_input, full_input, full_input};
  ID3D12Resource* uavs[kDlssNrUavCount] = {resources.GetWorkingInput()};

  codec->Dispatch(
      "PrepareModelInput", command.GetCommandList(), constants, srvs, uavs);

  command.Transition(command.GetCommandList(), resources.GetWorkingInput(),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  *out_model_color = resources.GetWorkingInput();
  *out_model_output = resources.GetWorkingOutput();
  *out_eval_width = work_width;
  *out_eval_height = work_height;
  return true;
}

bool Apply(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Resource* backbuffer,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params) {
  if (codec == nullptr || backbuffer == nullptr ||
      low_input == nullptr || low_enhanced == nullptr || params == nullptr) {
    return false;
  }

  DlssNrConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    constants.params[i] = params[i];
  }

  command.Transition(command.GetCommandList(), low_input,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command.GetCommandList(), low_enhanced,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kDlssNrSrvCount] = {backbuffer, low_input, low_enhanced};
  ID3D12Resource* uavs[kDlssNrUavCount] = {resources.GetCompositeOutput()};

  codec->Dispatch(
      "ComposeModelOutput", command.GetCommandList(), constants, srvs, uavs);

  command.CopyResourceToBackbuffer(
      command.GetCommandList(), resources.GetCompositeOutput(), backbuffer);
  command.Transition(command.GetCommandList(), low_enhanced,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_working_scale

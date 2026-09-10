#include "model_pipeline.h"

#include <cstring>

#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

bool PrepareInput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* full_input,
    ID3D12Resource** out_model_color,
    ID3D12Resource** out_model_output,
    unsigned int* out_eval_width,
    unsigned int* out_eval_height,
    unsigned int full_width,
    unsigned int full_height,
    unsigned int work_width,
    unsigned int work_height,
    const HdrUniforms* hdr) {
  if (out_model_color == nullptr || out_model_output == nullptr ||
      codec == nullptr || command_list == nullptr || full_input == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;
  (void)full_width;
  (void)full_height;

  const char* pass_name = "PrepareModelInput";
  if (hdr != nullptr) {
    pass_name = "TonemapForward";
    constants.params[0] = hdr->exposure;
    constants.params[1] = hdr->white_point;
    constants.params[2] = static_cast<float>(hdr->format);
  }

  state.Transition(command_list, full_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {full_input, full_input, full_input};
  ID3D12Resource* uavs[kUavCount] = {resources.GetWorkingInput()};

  codec->Dispatch(pass_name, command_list, constants, srvs, uavs);

  state.Transition(command_list, resources.GetWorkingInput(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  *out_model_color = resources.GetWorkingInput();
  *out_model_output = resources.GetWorkingOutput();
  *out_eval_width = work_width;
  *out_eval_height = work_height;
  return true;
}

bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* original,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params) {
  if (codec == nullptr || command_list == nullptr || original == nullptr ||
      low_input == nullptr || low_enhanced == nullptr || params == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    constants.params[i] = params[i];
  }

  state.Transition(command_list, original,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {original, low_input, low_enhanced};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(
      "ComposeModelOutput", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

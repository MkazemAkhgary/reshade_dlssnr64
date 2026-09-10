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
    bool use_working_scale) {
  if (out_model_color == nullptr || out_model_output == nullptr) return false;

  *out_model_color = full_input;
  *out_model_output = resources.GetOutput();
  *out_eval_width = full_width;
  *out_eval_height = full_height;

  if (!use_working_scale) return true;
  if (codec == nullptr || full_input == nullptr) return false;

  ComputeConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;

  ID3D12Resource* srvs[kSrvCount] = {full_input, full_input, full_input};
  ID3D12Resource* uavs[kUavCount] = {resources.GetWorkingInput()};

  codec->Dispatch("PrepareModelInput", command_list, constants, srvs, uavs);

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

  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {original, low_input, low_enhanced};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(
      "ComposeModelOutput", command_list, constants, srvs, uavs);
}

bool Apply(
    CommandContext& state,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* backbuffer,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params) {
  ID3D12GraphicsCommandList* list = state.GetCommandList();
  if (list == nullptr) return false;

  if (!ComposeOutput(state, list, resources, codec, backbuffer,
                     low_input, low_enhanced, full_width, full_height,
                     params)) {
    return false;
  }

  state.CopyResourceToBackbuffer(
      list, resources.GetCompositeOutput(), backbuffer);
  state.Transition(list, low_enhanced,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::model_pipeline

#include "model_pipeline.h"

#include <cstring>

#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

bool MeasureWhitePoint(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* full_input,
    unsigned int full_width,
    unsigned int full_height) {
  if (codec == nullptr || command_list == nullptr || full_input == nullptr ||
      resources.GetWhitePointBuffer() == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;

  state.Transition(command_list, full_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, resources.GetWhitePointBuffer(),
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {
      full_input, full_input, full_input, full_input};
  ID3D12Resource* uavs[kUavCount] = {resources.GetWhitePointBuffer()};

  // The white-point reduction produces an 8×8 tile buffer regardless of
  // frame size, so its grid is fixed at 1×1×1 (the shader's 8×8 thread
  // group is exactly that buffer). Without this override the dispatch
  // would launch one group per 8×8 tile of the frame — tens of thousands
  // of idle groups for 4K content.
  return codec->Dispatch(
      "MeasureWhitePoint", command_list, constants, srvs, uavs,
      /*threads_x=*/1, /*threads_y=*/1, /*threads_z=*/1);
}

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
    unsigned int work_height) {
  if (out_model_color == nullptr || out_model_output == nullptr ||
      codec == nullptr || command_list == nullptr || full_input == nullptr ||
      resources.GetWhitePointBuffer() == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;
  (void)full_width;
  (void)full_height;

  state.Transition(command_list, full_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, resources.GetWhitePointBuffer(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {
      full_input, full_input, full_input, resources.GetWhitePointBuffer()};
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
    ID3D12Resource* original_hdr,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params) {
  if (codec == nullptr || command_list == nullptr || original_hdr == nullptr ||
      low_input == nullptr || low_enhanced == nullptr || params == nullptr ||
      resources.GetWhitePointBuffer() == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    constants.params[i] = params[i];
  }

  state.Transition(command_list, original_hdr,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, resources.GetWhitePointBuffer(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {
      original_hdr, low_input, low_enhanced, resources.GetWhitePointBuffer()};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(
      "ComposeModelOutput", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

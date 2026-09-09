#include "dlssnr_stages_working_scale.h"

#include <cstring>

namespace reshade_dlssnr::dlssnr_stages_working_scale {

bool PrepareInput(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
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
  if (codec == nullptr) return false;

  if (!codec->InitDownsampleShader(device)) return false;

  if (full_input == nullptr) return false;

  DlssNrConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;

  codec->DispatchDownsample(
      command.GetCommandList(),
      constants,
      full_input,
      resources.GetWorkingInput());

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
    ID3D12Device* device,
    ID3D12Resource* backbuffer,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    float luma_strength,
    float chroma_strength,
    bool soft_compression,
    float soft_max_brighten,
    float soft_max_darken,
    float soft_max_chroma,
    float soft_knee) {
  if (codec == nullptr || backbuffer == nullptr ||
      low_input == nullptr || low_enhanced == nullptr) {
    return false;
  }

  if (!codec->InitUpsampleResidualShader(device)) return false;

  DlssNrConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  constants.params[0] = luma_strength;
  constants.params[1] = chroma_strength;
  constants.params[2] = soft_compression ? 1.0f : 0.0f;
  constants.params[3] = soft_max_brighten;
  constants.params[4] = soft_max_darken;
  constants.params[5] = soft_max_chroma;
  constants.params[6] = soft_knee;

  command.Transition(command.GetCommandList(), low_input,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command.GetCommandList(), low_enhanced,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  codec->DispatchUpsampleResidual(
      command.GetCommandList(),
      constants,
      backbuffer,
      low_input,
      low_enhanced,
      resources.GetCompositeOutput());

  command.CopyResourceToBackbuffer(
      command.GetCommandList(), resources.GetCompositeOutput(), backbuffer);
  command.Transition(command.GetCommandList(), low_enhanced,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_working_scale

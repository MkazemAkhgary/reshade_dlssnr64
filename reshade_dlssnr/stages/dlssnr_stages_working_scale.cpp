#include "dlssnr_stages_working_scale.h"

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
    unsigned int full_width,
    unsigned int full_height,
    unsigned int work_width,
    unsigned int work_height,
    bool use_working_scale,
    float luma_strength,
    float chroma_strength,
    float residual_suppression,
    float residual_threshold,
    bool soft_compression,
    float soft_max_brighten,
    float soft_max_darken,
    float soft_max_chroma,
    float soft_knee) {
  if (!use_working_scale || codec == nullptr || backbuffer == nullptr) {
    return false;
  }

  if (!codec->InitUpsampleResidualShader(device)) return false;

  DlssNrConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  constants.LumaStrength = luma_strength;
  constants.ChromaStrength = chroma_strength;
  constants.ResidualSuppression = residual_suppression;
  constants.ResidualThreshold = residual_threshold;
  constants.SoftCompression = soft_compression ? 1u : 0u;
  constants.SoftMaxBrighten = soft_max_brighten;
  constants.SoftMaxDarken = soft_max_darken;
  constants.SoftMaxChroma = soft_max_chroma;
  constants.SoftKnee = soft_knee;

  command.Transition(command.GetCommandList(), resources.GetWorkingOutput(),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command.GetCommandList(), resources.GetWorkingInput(),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  codec->DispatchUpsampleResidual(
      command.GetCommandList(),
      constants,
      backbuffer,
      resources.GetWorkingInput(),
      resources.GetWorkingOutput(),
      resources.GetCompositeOutput());

  command.CopyResourceToBackbuffer(
      command.GetCommandList(), resources.GetCompositeOutput(), backbuffer);
  command.Transition(command.GetCommandList(), resources.GetWorkingOutput(),
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_working_scale

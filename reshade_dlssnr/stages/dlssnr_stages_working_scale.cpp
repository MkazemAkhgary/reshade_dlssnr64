#include "dlssnr_stages_working_scale.h"

#include "dlssnr_stages_upscale.h"

namespace reshade_dlssnr::dlssnr_stages_working_scale {

bool RunDebugView(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
    ID3D12CommandQueue* command_queue,
    ID3D12Resource* backbuffer,
    ID3D12Resource* model_input,
    ID3D12Resource* model_output,
    unsigned int width,
    unsigned int height,
    int debug_view) {
  if (codec == nullptr || backbuffer == nullptr || command_queue == nullptr ||
      device == nullptr) {
    return false;
  }

  command.Reset();

  if (debug_view == 9) {
    if (!codec->InitDebugResidualShader(device)) return false;

    DlssNrConstants constants = {};
    constants.Mode = DlssNrMode_Resolve;
    constants.Width = width;
    constants.Height = height;

    command.Barrier(command.GetCommandList(), model_input,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Barrier(command.GetCommandList(), model_output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    codec->DispatchDebugResidual(
        command.GetCommandList(),
        constants,
        model_output,
        model_input,
        resources.composite_output);

    command.Barrier(command.GetCommandList(), resources.composite_output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    command.Barrier(command.GetCommandList(), backbuffer,
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    command.GetCommandList()->CopyResource(backbuffer, resources.composite_output);
    command.Barrier(command.GetCommandList(), backbuffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PRESENT);
    command.Barrier(command.GetCommandList(), resources.composite_output,
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    command.GetCommandList()->Close();
    ID3D12CommandList* lists[] = {command.GetCommandList()};
    command_queue->ExecuteCommandLists(1, lists);
    command.WaitGpu(command_queue);
    return true;
  }

  ID3D12Resource* debug_source =
      debug_view == 7 ? model_input : model_output;

  if (debug_source == nullptr) return false;

  const D3D12_RESOURCE_DESC source_desc = debug_source->GetDesc();
  const unsigned int source_width =
      static_cast<unsigned int>(source_desc.Width);
  const unsigned int source_height =
      static_cast<unsigned int>(source_desc.Height);

  if (!dlssnr_stages_upscale::Apply(
          command,
          resources,
          codec,
          device,
          debug_source,
          backbuffer,
          source_width,
          source_height,
          width,
          height)) {
    return false;
  }

  command.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command.GetCommandList()};
  command_queue->ExecuteCommandLists(1, lists);
  command.WaitGpu(command_queue);
  return true;
}

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
  *out_model_output = resources.output;
  *out_eval_width = full_width;
  *out_eval_height = full_height;

  if (!use_working_scale) return true;
  if (codec == nullptr) return false;

  if (!codec->InitDownsampleShader(device)) return false;

  if (full_input == nullptr) return false;

  DlssNrConstants constants = {};
  constants.Mode = DlssNrMode_Downsample;
  constants.Width = work_width;
  constants.Height = work_height;

  codec->DispatchDownsample(
      command.GetCommandList(),
      constants,
      full_input,
      resources.working_input);

  command.Barrier(command.GetCommandList(), resources.working_input,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  *out_model_color = resources.working_input;
  *out_model_output = resources.working_output;
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
    int debug_view) {
  if (!use_working_scale || codec == nullptr || backbuffer == nullptr) {
    return false;
  }

  if (!codec->InitUpsampleResidualShader(device)) return false;

  DlssNrConstants constants = {};
  constants.Mode = DlssNrMode_Resolve;
  constants.Width = full_width;
  constants.Height = full_height;
  constants.LumaStrength = luma_strength;
  constants.ChromaStrength = chroma_strength;
  constants.ResidualSuppression = residual_suppression;
  constants.ResidualThreshold = residual_threshold;

  command.Barrier(command.GetCommandList(), resources.working_output,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Barrier(command.GetCommandList(), resources.working_input,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  codec->DispatchUpsampleResidual(
      command.GetCommandList(),
      constants,
      backbuffer,
      resources.working_input,
      resources.working_output,
      resources.composite_output);

  command.Barrier(command.GetCommandList(), resources.composite_output,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
  command.Barrier(command.GetCommandList(), backbuffer,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  command.GetCommandList()->CopyResource(backbuffer, resources.composite_output);
  command.Barrier(command.GetCommandList(), backbuffer,
                  D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PRESENT);
  command.Barrier(command.GetCommandList(), resources.composite_output,
                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  command.Barrier(command.GetCommandList(), resources.working_output,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_working_scale

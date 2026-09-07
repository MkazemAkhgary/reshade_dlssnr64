#include "dlssnr_stages_final_composite.h"

namespace reshade_dlssnr::dlssnr_stages_final_composite {

bool Apply(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
    ID3D12Resource* backbuffer,
    unsigned int width,
    unsigned int height,
    bool use_selected_input,
    bool ui_mask_generated,
    float luma_strength,
    float chroma_strength,
    float white_point_scale) {
  if (codec == nullptr || backbuffer == nullptr || resources.output == nullptr) {
    return false;
  }

  ID3D12GraphicsCommandList* list = command.GetCommandList();

  if (use_selected_input && ui_mask_generated &&
      resources.ui_mask != nullptr && resources.composite_output != nullptr) {
    if (!codec->InitCompositeShader(device)) return false;

    DlssNrConstants constants = {};
    constants.Mode = DlssNrMode_Encode;
    constants.WhitePoint = white_point_scale;
    constants.Width = width;
    constants.Height = height;
    constants.Passthrough = 1u;
    constants.LumaStrength = luma_strength;
    constants.ChromaStrength = chroma_strength;

    command.Barrier(list, resources.output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Barrier(list, resources.ui_mask,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    codec->DispatchComposite(
        list,
        constants,
        resources.output,
        backbuffer,
        resources.ui_mask,
        resources.composite_output);

    command.Barrier(list, resources.composite_output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, resources.composite_output);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PRESENT);
    command.Barrier(list, resources.composite_output,
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Barrier(list, resources.ui_mask,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Barrier(list, resources.output,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }

  if (luma_strength != 1.0f || chroma_strength != 1.0f) {
    if (!codec->InitLumaColorShader(device)) return false;
    if (resources.composite_output == nullptr) return false;

    DlssNrConstants constants = {};
    constants.Mode = DlssNrMode_Encode;
    constants.WhitePoint = white_point_scale;
    constants.Width = width;
    constants.Height = height;
    constants.Passthrough = 1u;
    constants.LumaStrength = luma_strength;
    constants.ChromaStrength = chroma_strength;

    command.Barrier(list, resources.output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    codec->DispatchLumaColor(
        list,
        constants,
        resources.output,
        backbuffer,
        resources.composite_output);

    command.Barrier(list, resources.composite_output,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, resources.composite_output);
    command.Barrier(list, backbuffer,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PRESENT);
    command.Barrier(list, resources.composite_output,
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Barrier(list, resources.output,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }

  command.Barrier(list, resources.output,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
  command.Barrier(list, backbuffer,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(backbuffer, resources.output);
  command.Barrier(list, backbuffer,
                  D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PRESENT);
  command.Barrier(list, resources.output,
                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_final_composite

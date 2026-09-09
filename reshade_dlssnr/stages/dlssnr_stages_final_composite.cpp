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
    float chroma_strength) {
  if (codec == nullptr || backbuffer == nullptr || resources.GetOutput() == nullptr) {
    return false;
  }

  ID3D12GraphicsCommandList* list = command.GetCommandList();

  if (use_selected_input && ui_mask_generated &&
      resources.GetUiMask() != nullptr && resources.GetCompositeOutput() != nullptr) {
    if (!codec->InitCompositeShader(device)) return false;

    DlssNrConstants constants = {};
    constants.Mode = DlssNrMode_Encode;
    constants.Width = width;
    constants.Height = height;
    constants.LumaStrength = luma_strength;
    constants.ChromaStrength = chroma_strength;

    command.Transition(list, resources.GetOutput(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Transition(list, resources.GetUiMask(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    codec->DispatchComposite(
        list,
        constants,
        resources.GetOutput(),
        backbuffer,
        resources.GetUiMask(),
        resources.GetCompositeOutput());

    command.Transition(list, resources.GetCompositeOutput(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, resources.GetCompositeOutput());
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_PRESENT);
    command.Transition(list, resources.GetCompositeOutput(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Transition(list, resources.GetUiMask(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Transition(list, resources.GetOutput(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }

  if (luma_strength != 1.0f || chroma_strength != 1.0f) {
    if (!codec->InitLumaColorShader(device)) return false;
    if (resources.GetCompositeOutput() == nullptr) return false;

    DlssNrConstants constants = {};
    constants.Mode = DlssNrMode_Encode;
    constants.Width = width;
    constants.Height = height;
    constants.LumaStrength = luma_strength;
    constants.ChromaStrength = chroma_strength;

    command.Transition(list, resources.GetOutput(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    codec->DispatchLumaColor(
        list,
        constants,
        resources.GetOutput(),
        backbuffer,
        resources.GetCompositeOutput());

    command.Transition(list, resources.GetCompositeOutput(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, resources.GetCompositeOutput());
    command.Transition(list, backbuffer,
                       D3D12_RESOURCE_STATE_PRESENT);
    command.Transition(list, resources.GetCompositeOutput(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    command.Transition(list, resources.GetOutput(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }

  command.Transition(list, resources.GetOutput(),
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
  command.Transition(list, backbuffer,
                     D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(backbuffer, resources.GetOutput());
  command.Transition(list, backbuffer,
                     D3D12_RESOURCE_STATE_PRESENT);
  command.Transition(list, resources.GetOutput(),
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_final_composite

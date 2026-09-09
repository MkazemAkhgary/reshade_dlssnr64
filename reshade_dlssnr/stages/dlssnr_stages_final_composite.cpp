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
    float luma_strength,
    float chroma_strength) {
  if (codec == nullptr || backbuffer == nullptr || resources.GetOutput() == nullptr) {
    return false;
  }

  ID3D12GraphicsCommandList* list = command.GetCommandList();

  if (luma_strength != 1.0f || chroma_strength != 1.0f) {
    if (!codec->InitLumaColorShader(device)) return false;
    if (resources.GetCompositeOutput() == nullptr) return false;

    DlssNrConstants constants = {};
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

    command.CopyResourceToBackbuffer(list, resources.GetCompositeOutput(), backbuffer);
    command.Transition(list, resources.GetOutput(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
  }

  command.CopyResourceToBackbuffer(list, resources.GetOutput(), backbuffer);
  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_final_composite

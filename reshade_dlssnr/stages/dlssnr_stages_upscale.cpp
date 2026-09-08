#include "dlssnr_stages_upscale.h"

namespace reshade_dlssnr::dlssnr_stages_upscale {

bool Apply(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
    ID3D12Resource* source,
    ID3D12Resource* backbuffer,
    unsigned int source_width,
    unsigned int source_height,
    unsigned int target_width,
    unsigned int target_height) {
  if (codec == nullptr || source == nullptr || backbuffer == nullptr ||
      device == nullptr) {
    return false;
  }

  if (!codec->InitDownsampleShader(device)) return false;

  DlssNrConstants constants = {};
  constants.Mode = DlssNrMode_Downsample;
  constants.Width = target_width;
  constants.Height = target_height;

  codec->DispatchDownsample(
      command.GetCommandList(),
      constants,
      source,
      resources.composite_output);

  command.Transition(command.GetCommandList(), resources.composite_output,
                     D3D12_RESOURCE_STATE_COPY_SOURCE);
  command.Transition(command.GetCommandList(), backbuffer,
                     D3D12_RESOURCE_STATE_COPY_DEST);
  command.GetCommandList()->CopyResource(backbuffer, resources.composite_output);
  command.Transition(command.GetCommandList(), backbuffer,
                     D3D12_RESOURCE_STATE_PRESENT);
  command.Transition(command.GetCommandList(), resources.composite_output,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_upscale

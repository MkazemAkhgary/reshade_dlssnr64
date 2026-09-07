#include "dlssnr_stages_ui_mask.h"

namespace reshade_dlssnr::dlssnr_stages_ui_mask {

bool Generate(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
    ID3D12Resource* pre_ui_source,
    ID3D12Resource* backbuffer,
    unsigned int width,
    unsigned int height,
    float threshold,
    float softness) {
  if (codec == nullptr || pre_ui_source == nullptr ||
      backbuffer == nullptr || resources.ui_mask == nullptr) {
    return false;
  }

  if (!codec->InitMaskShader(device)) {
    return false;
  }

  DlssNrConstants constants = {};
  constants.Mode = DlssNrMode_Encode;
  constants.WhitePoint = 1.0f;
  constants.Width = width;
  constants.Height = height;
  constants.Passthrough = 1u;
  constants.UiMaskThreshold = threshold;
  constants.UiMaskSoftness = softness;

  codec->DispatchMask(
      command.GetCommandList(),
      constants,
      pre_ui_source,
      backbuffer,
      resources.ui_mask);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_ui_mask

#pragma once

#include <d3d12.h>

#include "codec.h"
#include "dlssnr_command_context.h"
#include "dlssnr_pass_resources.h"

namespace reshade_dlssnr::dlssnr_stages_upscale {

bool Apply(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Device* device,
    ID3D12Resource* source,
    ID3D12Resource* backbuffer,
    unsigned int target_width,
    unsigned int target_height);

}  // namespace reshade_dlssnr::dlssnr_stages_upscale

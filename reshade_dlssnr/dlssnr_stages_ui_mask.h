#pragma once

#include <d3d12.h>

#include "codec.h"
#include "dlss_guides.h"
#include "dlssnr_command_context.h"
#include "dlssnr_pass_resources.h"

namespace reshade_dlssnr::dlssnr_stages_ui_mask {

bool Generate(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12Resource* pre_ui_source,
    ID3D12Resource* backbuffer,
    unsigned int width,
    unsigned int height,
    float threshold,
    float softness);

}  // namespace reshade_dlssnr::dlssnr_stages_ui_mask

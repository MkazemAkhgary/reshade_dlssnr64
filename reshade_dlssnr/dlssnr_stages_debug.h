#pragma once

#include <d3d12.h>

#include "codec.h"
#include "dlss_guides.h"
#include "dlssnr_command_context.h"
#include "dlssnr_pass_resources.h"

namespace reshade_dlssnr::dlssnr_stages_debug {

bool Run(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12CommandQueue* command_queue,
    ID3D12Resource* backbuffer,
    const DlssGuideState& guides,
    ID3D12Resource* model_input,
    unsigned int width,
    unsigned int height,
    int debug_view);

}  // namespace reshade_dlssnr::dlssnr_stages_debug

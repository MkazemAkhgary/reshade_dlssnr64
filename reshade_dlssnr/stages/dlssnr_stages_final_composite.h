#pragma once

#include <d3d12.h>

#include "codec.h"
#include "dlssnr_command_context.h"
#include "dlssnr_pass_resources.h"

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
    float chroma_strength);

}  // namespace reshade_dlssnr::dlssnr_stages_final_composite

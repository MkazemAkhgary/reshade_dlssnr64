#pragma once

#include <d3d12.h>

#include "codec.h"
#include "dlssnr_command_context.h"
#include "dlssnr_pass_resources.h"

namespace reshade_dlssnr::dlssnr_stages_working_scale {

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
    bool use_working_scale);

bool RunDebugView(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12CommandQueue* command_queue,
    ID3D12Resource* backbuffer,
    ID3D12Resource* model_input,
    int debug_view);

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
    float white_point_scale,
    int debug_view);

}  // namespace reshade_dlssnr::dlssnr_stages_working_scale

#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

bool PrepareInput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
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

// Composes the model output into PassResources::GetCompositeOutput().
// Callers then decide where to copy it (backbuffer, DLSS output, etc.).
bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* original,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params);

// Convenience wrapper for the Present path: composes and copies the result
// into the swapchain backbuffer.
bool Apply(
    CommandContext& state,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* backbuffer,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params);

}  // namespace reshade_dlssnr::model_pipeline

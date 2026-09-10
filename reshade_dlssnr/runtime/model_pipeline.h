#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

// Resizes the source into the working resolution. The model always runs at
// `work_width × work_height`; the caller chooses whether that differs from
// the frame size via the working-scale slider.
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
    unsigned int work_height);

// Composes the model output into PassResources::GetCompositeOutput().
// Callers then decide where to copy it.
bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* original_hdr,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params);

}  // namespace reshade_dlssnr::model_pipeline

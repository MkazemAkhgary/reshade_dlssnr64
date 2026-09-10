#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

// Reduces the source to a per-tile max luma buffer used by the other passes
// to compute the white point. Must run before PrepareInput each frame.
bool MeasureWhitePoint(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* full_input,
    unsigned int full_width,
    unsigned int full_height);

// Normalizes `full_input` to SDR using the white point buffer and writes the
// result into the working resources. When `use_working_scale` is false the
// working resources are still used (at full resolution) so the model always
// receives a normalized SDR image.
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

// Composes the model output into PassResources::GetCompositeOutput(),
// remapping back to HDR. Callers then decide where to copy it.
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

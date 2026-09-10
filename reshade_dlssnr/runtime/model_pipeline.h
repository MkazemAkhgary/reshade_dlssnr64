#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

// HDR tonemap uniforms. When passed to PrepareInput, the input is
// tonemapped from HDR to SDR before the model sees it. When nullptr, the
// input is a plain resample.
//
// `format` follows the shader convention: 0 = SDR sRGB, 1 = scRGB linear,
// 2 = HDR10 PQ.
struct HdrUniforms {
  float exposure = 1.0f;
  float white_point = 4.0f;
  uint32_t format = 0;
};

// Prepares the model input. With `hdr == nullptr`, resamples `full_input`
// into the working resolution. With `hdr != nullptr`, additionally
// tonemaps HDR → SDR. In both cases the model always runs at
// `work_width × work_height`.
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
    const HdrUniforms* hdr = nullptr);

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

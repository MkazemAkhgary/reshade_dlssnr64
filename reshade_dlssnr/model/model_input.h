// Model input preparation. Resamples the source into the working-scale buffer
// the model reads, optionally tonemapping HDR -> SDR first.
#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

// HDR tonemap uniforms, written at kHdrSlotBase in ComputeConstants::params.
// Zero-initialised `format` means SDR sRGB, 1 scRGB, 2 HDR10 PQ.
struct HdrUniforms {
  float exposure = 1.0f;
  float tonemap_white = 4.0f;
  uint32_t format = 0;
};

// With `hdr == nullptr`, plain resample. With `hdr != nullptr`, additionally
// tonemaps HDR -> SDR. Model always runs at `work_width x work_height`.
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

}  // namespace reshade_dlssnr::model_pipeline

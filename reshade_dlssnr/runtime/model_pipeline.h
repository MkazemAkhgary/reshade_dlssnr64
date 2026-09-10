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
  float edit_strength = 1.0f;
  // How the model's edit is transferred back to HDR:
  //   0 = additive (RGB delta in SDR, then inverse-tonemapped)
  //   1 = ratio (multiplicative per-channel in linear SDR, applied
  //       directly to the HDR linear)
  uint32_t chroma_mode = 0;
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
// Callers then decide where to copy it. With `hdr == nullptr`, dispatches
// the SDR composition. With `hdr != nullptr`, dispatches the HDR
// composition, which inverse-tonemaps the model's SDR edit back into the
// HDR source's own range and encoding.
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
    const float* params,
    const HdrUniforms* hdr = nullptr);

}  // namespace reshade_dlssnr::model_pipeline

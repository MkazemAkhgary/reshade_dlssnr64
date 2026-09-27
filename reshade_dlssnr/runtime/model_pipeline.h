#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"
#include "settings.h"

namespace reshade_dlssnr::model_pipeline {

// HDR tonemap uniforms. When passed to PrepareInput, the input is
// tonemapped from HDR to SDR before the model sees it. When nullptr, the
// input is a plain resample.
//
// `format` follows the shader convention: 0 = SDR sRGB, 1 = scRGB linear,
// 2 = HDR10 PQ.
//
// PrepareInput writes all three to ComputeConstants::params at kHdrSlotBase,
// in this order, so they occupy three consecutive floats. The shader side
// spells that out as `gParams[0]` in kTonemapForward, which keeps its own
// layout.
//
// ComposeOutput writes all three at kHdrSlotBase + 0/1/2, read as
// gParams[8].xyz in kComposeModelOutputHdr. Exposure and white point are read
// because the compose inverts the tonemap on the model's output: the model
// saw the tonemapped image, so the ratio it implies is in that domain. See
// the region map in compute_engine.h before changing any of this.
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
//
// The model's edit is carried onto `original` as a per-channel ratio, which
// is scale-invariant and therefore means the same thing whatever the
// original's range is. `edit_strength` scales it before application; 0 makes
// the compose a pass-through.
//
// With `hdr == nullptr`, dispatches the SDR composition, which applies the
// ratio in the source's own sRGB-encoded space. With `hdr != nullptr`,
// dispatches the HDR composition, which decodes to linear, applies the ratio
// there, and re-encodes to the source's format.
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
    float edit_strength = 1.0f,
    bool rcas_enabled = false,
    float rcas_strength = 0.5f,
    const HdrUniforms* hdr = nullptr);

// Applies a grade to `target` in place, reading it through `scratch`.
//
// In place because every call site wants the result where the original was:
// the model reads the working input, the compose step publishes the
// composite, and the Release stage writes the backbuffer. A compute dispatch
// cannot read and write one resource, so the source is copied into `scratch`
// first and the dispatch reads that. The copy is the price of grading in
// place, and it is cheaper than threading an override pointer through every
// consumer of each of those three images.
//
// `scratch` must be a UAV-capable texture at the same size and format as
// `target`, and must not be one the caller is already using for the frame.
// Where it comes from is the caller's business: a pass uses its grade scratch
// slot, the Release stage uses the controller's.
//
// `format` is the HdrFormat of the target, and the grade needs it: it decodes
// the image to linear light, grades there, and re-encodes, so it has to know
// which of the three encodings it is looking at. Every target here tracks the
// backbuffer's encoding, so the caller resolves it once from HdrState.
//
// A disabled grade is a no-op that returns true, so callers do not have to
// test `enabled` themselves — but they should still skip the resource setup
// that only a graded frame needs.
//
// Leaves `target` in UNORDERED_ACCESS. The caller transitions it back to
// whatever its own pipeline expects.
bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                HdrFormat format,
                const GradeSettings& grade);

}  // namespace reshade_dlssnr::model_pipeline

// Composes the model's edit into PassResources::GetCompositeOutput(), in SDR
// or HDR depending on whether `hdr` is set.
#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"
#include "model/model_input.h"
#include "compose/settings.h"
#include "grade/settings.h"

namespace reshade_dlssnr::model_pipeline {

bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* packed_source,
    ID3D12Resource* hdr_source,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const StrengthSettings& strength,
    const ResidualSettings& residual = {},
    const HdrUniforms* hdr = nullptr,
    bool apply_edit_to_pre_grade = false);

}  // namespace reshade_dlssnr::model_pipeline

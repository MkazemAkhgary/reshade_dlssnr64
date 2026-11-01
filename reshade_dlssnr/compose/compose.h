// Composes the model's edit into PassResources::GetCompositeOutput(), in SDR
// or HDR depending on whether `hdr` is set.
//
// The residual is the ratio between `low_input` and `low_enhanced`. When
// `lift_input` and `lift_enhanced` are given, a second ratio between them is
// multiplied in: the local-exposure lift the model input carries but the
// pre-grade base does not.
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
    bool apply_edit_to_pre_grade = false,
    ID3D12Resource* lift_input = nullptr,
    ID3D12Resource* lift_enhanced = nullptr);

}  // namespace reshade_dlssnr::model_pipeline

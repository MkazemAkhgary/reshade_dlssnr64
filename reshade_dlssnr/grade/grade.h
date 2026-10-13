// Grade application. Grades `target` in place, through `scratch`.
#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "grade/settings.h"
#include "hdr/settings.h"

namespace reshade_dlssnr::model_pipeline {

// In place because every call site wants the result where the original was.
// A compute dispatch cannot read and write one resource, so the source is
// copied into `scratch` first and the dispatch reads that. Leaves `target`
// in UNORDERED_ACCESS.
//
// A disabled grade is a no-op that returns true.
// exposure_map is optional; when set, the decoded source is divided by the
// local exposure scale before grading, regardless of grade.enabled.
bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                int format_code,
                const GradeSettings& grade,
                ID3D12Resource* exposure_map = nullptr,
                float unexpose_naturalness = 0.2f,
                float unexpose_reference_l = 0.5f);

}  // namespace reshade_dlssnr::model_pipeline

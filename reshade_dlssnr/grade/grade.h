// Grade application. Grades `target` in place, through `scratch`.
#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "grade/settings.h"
#include "hdr/settings.h"

namespace reshade_dlssnr::model_pipeline {

// Writes the grade transform to the shared compute constants. The caller
// supplies its own domain-specific format/exposure flags.
void SetGradeConstants(ComputeConstants& constants, const GradeSettings& grade);


// In place because every call site wants the result where the original was.
// A compute dispatch cannot read and write one resource, so the source is
// copied into `scratch` first and the dispatch reads that. Leaves `target`
// in UNORDERED_ACCESS.
//
// A disabled grade is a no-op unless local/global carriers are supplied.
// When local/global carriers are supplied, the decoded source is divided by
// a multiplier interpolated in log exposure space between the exact per-pixel
// local multiplier and an optional 1x1 global multiplier before grading.
// restore_mode 0 uses per-pixel restoration; 1 uses the global scalar.
bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                int format_code,
                const GradeSettings& grade,
                ID3D12Resource* local_carrier = nullptr,
                ID3D12Resource* global_carrier = nullptr,
                ID3D12Resource* global_scale = nullptr,
                float restore_mode = 1.0f,
                float restore_strength = 1.0f);

}  // namespace reshade_dlssnr::model_pipeline

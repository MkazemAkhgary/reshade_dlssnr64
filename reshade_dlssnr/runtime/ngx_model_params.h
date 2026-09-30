// Shared DLSSNR model tuning parameters for feature creation and evaluation.
#pragma once

#include "ngx_parameter_block.h"
#include "state/settings.h"

namespace reshade_dlssnr {

// Applies the per-pass model tuning values (preset, intensity, structure,
// tone, masks, UI correction) to the given NGX parameter block. Called both
// at feature creation and at evaluation so the two paths stay in sync.
void ApplyModelTuningParams(NgxParameterBlock& block, const PassSettings& pass);

}  // namespace reshade_dlssnr

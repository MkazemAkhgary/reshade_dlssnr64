// Shared DLSSNR model tuning parameters for feature creation and evaluation.
#pragma once

#include "feature/pass/view.h"
#include "model/ngx_parameter_block.h"

namespace reshade_dlssnr {

// Applies the per-pass model tuning values (preset, intensity, structure,
// tone, masks, UI correction) to the given NGX parameter block. Called both
// at feature creation and at evaluation so the two paths stay in sync.
//
// It takes the pass snapshot, not the whole PassSettings: the model tuning is
// the only part of a pass NGX ever sees, and taking the snapshot is what lets
// this stay independent of where the pass's settings are stored.
void ApplyModelTuningParams(NgxParameterBlock& block,
                            const feature::pass::PassSnapshot& pass);

}  // namespace reshade_dlssnr

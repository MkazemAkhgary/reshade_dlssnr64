// Settings trace: emits a change log when DLSSNR_TRACE is enabled.
#pragma once

#include "state/settings.h"

namespace reshade_dlssnr {

// Dumps the settings as they will run, read from the store. The pass rows
// come from pass_order and are keyed by stable pass id, so a reorder shows
// up as a relabelled row rather than as a value moving.
void TraceSettings(const Settings& settings);

}  // namespace reshade_dlssnr

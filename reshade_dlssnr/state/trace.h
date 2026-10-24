// Settings trace: emits a change log when DLSSNR_TRACE is enabled.
#pragma once

#include "pipeline/pipeline_trace.h"
#include "state/settings.h"

#if DLSSNR_TRACE_ENABLED

namespace reshade_dlssnr {

// Dumps the settings as they will run, read from the store. The pass rows
// come from pass_order and are keyed by stable pass id, so a reorder shows
// up as a relabelled row rather than as a value moving.
void TraceSettings(const Settings& settings);

}  // namespace reshade_dlssnr

#define TRACE_SETTINGS(settings) ::reshade_dlssnr::TraceSettings(settings)

#else

#define TRACE_SETTINGS(settings) ((void)0)

#endif  // DLSSNR_TRACE_ENABLED

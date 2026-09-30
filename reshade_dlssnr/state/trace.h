// Settings trace: emits a change log when DLSSNR_TRACE is enabled.
#pragma once

#include "state/settings.h"

namespace reshade_dlssnr {

// `resolved` is kMaxPasses entries, the inheritance-applied view the runtime
// reads. Compared against the raw pass settings so the dump can suppress
// resolved fields that inheritance did not touch.
void TraceSettings(const Settings& settings, const PassSettings* resolved);

}  // namespace reshade_dlssnr

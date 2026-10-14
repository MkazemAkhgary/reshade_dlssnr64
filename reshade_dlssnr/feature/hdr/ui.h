// HDR non-row UI.
//
// The settings are rows; the tab bar, its bodies and the diagnostics readout
// are not. They are declared as hooks in schema.cpp and implemented here, so
// the HDR feature owns its rendering the same way it owns its settings.

#pragma once

#include <cstdint>
#include <string>

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
struct DrawOptions;
}  // namespace ctl

namespace feature {
namespace hdr {

// Draw the three-tab exposure layer bar and each open tab's body, then the
// stack-order note. Signature matches ctl::HookDraw so it can be passed
// straight to AddHook*. The layer bodies are drawn for the same instance key
// the HDR section is being drawn for, which the hook receives and forwards.
bool DrawExposureTabs(const void* context, ctl::ValueStore& store,
                      const ctl::DrawOptions& options,
                      const std::string& section, const ctl::InstanceKey& key,
                      bool& changed);

// Live HDR measurement readout, drawn at the end of the HDR section.
bool DrawHdrDiagnostics(const void* context, ctl::ValueStore& store,
                        const ctl::DrawOptions& options,
                        const std::string& section,
                        const ctl::InstanceKey& key, bool& changed);

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

// HDR non-row UI.
//
// The settings are rows; the layer collapsibles and the diagnostics readout
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

// Draw the three exposure layers as nested collapsibles, with the stack-order
// note below them. Each collapsible's open state is persisted via
// feature::ui_state, so the layers are independent of each other and of the
// parent HDR section. The layer bodies are drawn for the same instance key the
// HDR section is being drawn for, which the hook receives and forwards.
bool DrawHdrLayers(const void* context, ctl::ValueStore& store,
                   const ctl::DrawOptions& options,
                   const std::string& section, const ctl::InstanceKey& key,
                   bool& changed);

// Live HDR measurement readout, drawn at the end of the HDR section.
bool DrawHdrDiagnostics(const void* context, ctl::ValueStore& store,
                        const ctl::DrawOptions& options,
                        const std::string& section,
                        const ctl::InstanceKey& key, bool& changed);

// Read-only k2/k4 readout, drawn under the k1/k3/inflection sliders.
bool DrawBt2446Derived(const void* context, ctl::ValueStore& store,
                       const ctl::DrawOptions& options,
                       const std::string& section,
                       const ctl::InstanceKey& key, bool& changed);

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

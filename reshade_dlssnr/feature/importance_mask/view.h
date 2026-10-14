// Importance Mask view: read the section out of the store as an
// ImportanceMaskSettings.
//
// The store is the source of truth; the mask builder wants the plain struct it
// already takes. This is the one place that translates between them, so the
// setting spellings appear once and the runtime never spells one.
//
// The section is single-instance, so the reader takes the same key its section
// draws with.

#pragma once

#include "importance_mask/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace importance_mask {

// The snapshot is the whole ImportanceMaskSettings, resolved from the store.
// Naming it separately is what lets the render path take a snapshot and never
// an ImportanceMaskSettings that might have come from anywhere.
using ImportanceMaskSnapshot = ImportanceMaskSettings;

ImportanceMaskSnapshot Read(const ctl::ValueStore& store,
                            const ctl::InstanceKey& key);

}  // namespace importance_mask
}  // namespace feature
}  // namespace reshade_dlssnr

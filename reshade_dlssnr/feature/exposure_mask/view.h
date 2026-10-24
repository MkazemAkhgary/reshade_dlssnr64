// Exposure Mask view: read the section out of the store as an
// ExposureMaskSettings.
//
// The store is the source of truth; the mask builder wants the plain struct it
// already takes. This is the one place that translates between them, so the
// setting spellings appear once and the runtime never spells one.
//
// The section is pass-keyed, so the reader takes the same stable pass key its section draws with.

#pragma once

#include "exposure_mask/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace exposure_mask {

// The snapshot is the whole ExposureMaskSettings, resolved from the store.
// Naming it separately is what lets the render path take a snapshot and never
// an ExposureMaskSettings that might have come from anywhere.
using ExposureMaskSnapshot = ExposureMaskSettings;

ExposureMaskSnapshot Read(const ctl::ValueStore& store,
                            const ctl::InstanceKey& key);

}  // namespace exposure_mask
}  // namespace feature
}  // namespace reshade_dlssnr

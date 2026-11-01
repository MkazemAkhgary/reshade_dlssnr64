// HDR view: read the HDR sections out of the store as an HdrSettings.
//
// The store is the source of truth; the runtime wants the plain HdrSettings it
// already passes around. This is the one place that translates between them,
// so the setting spellings appear once and the runtime never spells one.
//
// The sections are all pass-keyed and independent: HDR itself, and the two
// global exposure layers. Each layer owns an ExposureFilterSettings, so the
// filter rows are read here rather than by the fragment.
//
// Local exposure is not one of them. It is a pass of its own and its view lives
// in feature/local_exposure.

#pragma once

#include "hdr/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace hdr {

// The HDR snapshot is the whole HdrSettings, resolved from the store. Naming
// it separately is what lets the render path take a snapshot and never a
// HdrSettings that might have come from anywhere.
using HdrSnapshotView = HdrSettings;

// Read every HDR section for `key` (the stable pass ID used by the sections).
HdrSnapshotView Read(const ctl::ValueStore& store,
                     const ctl::InstanceKey& key);

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

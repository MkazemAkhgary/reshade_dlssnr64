// HDR view: read the four HDR sections out of the store as an HdrSettings.
//
// The store is the source of truth; the runtime wants the plain HdrSettings it
// already passes around. This is the one place that translates between them,
// so the setting spellings appear once and the runtime never spells one.
//
// The four sections are all pass-keyed and independent: HDR itself, and
// the three exposure layers. Each layer owns an ExposureFilterSettings, so
// the filter rows are read here rather than by the fragment.

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

// Read all four sections for `key` (the stable pass ID used by the HDR sections).
HdrSnapshotView Read(const ctl::ValueStore& store,
                     const ctl::InstanceKey& key);

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

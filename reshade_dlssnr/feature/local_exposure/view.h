// Local exposure view: read the section out of the store as a
// LocalExposureSettings.
//
// The store is the source of truth; the runtime wants the plain struct it
// already passes around. This is the one place that translates between them, so
// the setting spellings appear once and the runtime never spells one.

#pragma once

#include "local_exposure/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace local_exposure {

LocalExposureSettings Read(const ctl::ValueStore& store,
                           const ctl::InstanceKey& key);

}  // namespace local_exposure
}  // namespace feature
}  // namespace reshade_dlssnr

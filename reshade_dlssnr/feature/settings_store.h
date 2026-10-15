// Application-owned composition of feature schemas and their shared setting store.
#pragma once

#include "ctl/values.h"

namespace reshade_dlssnr {
namespace feature {

ctl::ValueStore& SettingsStore();

}  // namespace feature
}  // namespace reshade_dlssnr

// Application-owned composition of feature schemas, their shared setting
// store, and the commands they contribute to the context menu.
#pragma once

#include "ctl/menu.h"
#include "ctl/values.h"

namespace reshade_dlssnr {
namespace feature {

ctl::ValueStore& SettingsStore();

// Built once from RegisterAllMenuCommands, like the store is built from
// RegisterAllSchemas.
ctl::MenuCommandRegistry& MenuCommands();

}  // namespace feature
}  // namespace reshade_dlssnr

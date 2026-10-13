// The one ValueStore, and nothing else.
//
// The store owns the schema and every value the user can edit, so it is the
// single source of truth for settings. It is a function-local static: built on
// first use, destroyed at exit, and reachable from anywhere without a global
// initialisation order problem.
//
// This is deliberately the whole file. It does not know about Settings, the
// UI, or any feature -- it just makes the store and hands it out.

#pragma once

#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

// The process-wide store. On first call, builds the schema from every feature's
// RegisterXxxSchema and freezes it. If the schema fails to build, the returned
// store has an unbuilt schema; callers check `schema().built()` before drawing.
ValueStore& Store();

}  // namespace ctl
}  // namespace reshade_dlssnr

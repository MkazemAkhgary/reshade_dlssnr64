// The explicit, ordered list of features that make up the settings UI.
//
// This file is the only place that knows features exist as a set. It owns an
// ordered list of registration calls -- one line per feature -- and nothing
// else. There is no descriptor table, because there is no need to enumerate
// features at run time; if that need ever appears (a debug panel listing
// features, a diagnostics dump), add a {name, register_fn} view *beside* this
// list, keeping the explicit calls the source of truth. Such a table must not
// grow to carry section names, hook lists, dependencies or enable flags --
// those belong inside each feature's own registration function.
//
// No static self-registration anywhere: initialization order across
// translation units is unspecified, registration would be invisible at the
// call site, and there would be no way to build a subset for tests.

#pragma once

#include "ctl/schema.h"

namespace reshade_dlssnr {
namespace feature {

// Declare every feature's sections, settings, metadata, hooks, enable
// predicates and reset callbacks. Registration order is render order.
//
// Each feature exposes exactly one function, RegisterXxxSchema(Schema&), and
// declares everything about its settings inside it, inline at the declaration
// site.
void RegisterAllSchemas(ctl::Schema& schema);

}  // namespace feature
}  // namespace reshade_dlssnr

// Grade view: read one grade instance out of the store as a GradeSettings.
//
// The store is the source of truth for settings; the runtime wants a plain
// GradeSettings. This is the one place that translates between them, so the
// setting spellings ("black_level", "luma_mode", ...) appear exactly once and
// the runtime never has to know them.
//
// A view reads; it does not write. The UI writes through the store's own
// Write, and the runtime only ever observes.
//
// `section` is the section the grade is declared on -- "Grade Backbuffer" for
// the backbuffer grade, or a pass's grade section -- and `prefix` is the name
// prefix grade_rows::Declare was given for that instance ("" for a section
// with a single grade, "grade_input." / "grade_output." for the two grades a
// pass hosts).

#pragma once

#include <string>

#include "grade/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace grade {

// Read the grade named by `section` + `prefix` for `key`.
GradeSettings Read(const ctl::ValueStore& store, const ctl::InstanceKey& key,
                   const std::string& section, const std::string& prefix = {});

// ---------------------------------------------------------------------------
// Per-consumer snapshot
// ---------------------------------------------------------------------------

// What the runtime holds for a grade. It is a plain value, built once where
// the store is reachable and then passed by reference down the render path,
// so nothing below the snapshot boundary needs the store, the key, or a
// setting name.
//
// It is deliberately the same shape as GradeSettings -- the snapshot is the
// seam, not a second representation -- but naming it separately is what lets
// the render path take a `GradeSnapshot` and never a `GradeSettings` that
// might have come from anywhere.
using GradeSnapshot = GradeSettings;

// Read the backbuffer grade. It is the one grade with a section to itself, so
// it takes the bare names.
GradeSnapshot ReadBackbuffer(const ctl::ValueStore& store);

}  // namespace grade
}  // namespace feature
}  // namespace reshade_dlssnr

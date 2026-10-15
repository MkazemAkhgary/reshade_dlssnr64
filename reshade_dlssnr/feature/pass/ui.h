// Pass non-row interface.
//
// The pass section is instanced; its rows are plain settings declared in
// schema.cpp. What is not a row is pass identity: which store key a pass
// uses, and how new pass ids are allocated. Identity lives here so the
// schema, the store, and the runtime all agree on one key definition.

#pragma once

#include <cstdint>

#include <vector>

#include "ctl/values.h"

namespace reshade_dlssnr {

namespace feature {
namespace pass {

// A pass's identity, independent of its position in the tab order.
//
// Monotonic and never reused. Allocation is handled by NewPassId() in
// state/settings so the high-water mark can be persisted.
using PassId = uint32_t;

// The store key for pass `id`. Positional pass numbers are NOT valid input:
// a reorder would silently relabel every setting that followed it.
inline ctl::InstanceKey PassKey(PassId id) {
  return ctl::InstanceKey::FromInt(static_cast<int64_t>(id));
}

// Every pass id currently in play, in the order the passes sit.
//
// The canonical order is persisted as a ctl integer-list setting. This
// vector is the runtime/UI mirror: adding a pass appends an id, removing a
// pass erases it, and each pass keeps its settings under a stable key.
using PassOrder = std::vector<PassId>;

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

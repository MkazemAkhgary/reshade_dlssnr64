// Pass non-row interface.
//
// The pass section is instanced; its rows are plain settings declared in
// schema.cpp. What is not a row is the identity of an instance: which pass a
// key belongs to, and how a pass comes to hold a key at all. That lives here,
// beside the schema that defines the key, and not in settings.h -- which
// carries names and nothing else.

#pragma once

#include <cstdint>

#include <vector>

namespace reshade_dlssnr {
namespace ctl {
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace pass {

// A pass's identity, independent of its position in the tab order.
//
// Monotonic and never reused: ids come from a per-process counter, and one
// that has been handed out stays retired even after the pass is deleted. That
// is what makes a reorder free -- see the note on `pass_order`.
using PassId = uint32_t;

// The store key for pass `id`. Positional pass numbers are NOT valid input:
// a reorder would silently relabel every setting that followed it.
ctl::InstanceKey PassKey(PassId id);

// The next unused pass id. Monotonic across adds; removing a pass never makes
// its id available again, so a removed pass's settings can never be mistaken
// for a new pass's.
PassId NewPassId();

// Every pass id ever handed out, in the order the passes currently sit.
//
// This is application state, deliberately: ctl does not track instance count
// and does not track instance order either. Reordering reorders this vector
// and nothing else -- no store value moves, and the parent links are recomputed
// from the new order. Adding a pass appends an id; removing one erases it.
//
// The store is unaffected by any of it, which is the whole point of stable keys.
using PassOrder = std::vector<PassId>;

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

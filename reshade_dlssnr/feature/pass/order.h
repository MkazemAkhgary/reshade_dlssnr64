// Stable pass identities and the inheritance chain derived from tab order.
#pragma once

#include "ctl/values.h"
#include "feature/pass/ui.h"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

// Re-link the store's parent chain from tab order without moving any values.
void ApplyOrder(ctl::ValueStore& store, const PassOrder& order);

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

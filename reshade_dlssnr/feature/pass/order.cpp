#include "feature/pass/order.h"

#include "feature/pass/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

ctl::InstanceKey PassKey(PassId id) {
  return ctl::InstanceKey::FromInt(static_cast<int64_t>(id));
}

PassId NewPassId() {
  static PassId next_id = 1;
  return next_id++;
}

void ApplyOrder(ctl::ValueStore&, const PassOrder&) {
  // Inheritance has been removed from ctl. Pass order no longer affects
  // store state. This stub keeps the addon building until the call sites
  // and this file are deleted in the next step.
}

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

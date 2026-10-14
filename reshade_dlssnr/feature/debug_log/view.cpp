#include "feature/debug_log/view.h"

#include "ctl/values.h"
#include "feature/debug_log/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace debug_log {

int Read(const ctl::ValueStore& store, const ctl::InstanceKey& key) {
  const int value = store.Read(kSection, key, kDebugView).as_int;
  return value < 0 ? 0 : value;
}

}  // namespace debug_log
}  // namespace feature
}  // namespace reshade_dlssnr

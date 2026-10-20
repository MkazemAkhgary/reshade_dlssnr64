#include "feature/settings_store.h"
#include "profiling/profile.h"

#include "feature/register_all.h"

namespace reshade_dlssnr {
namespace feature {

ctl::ValueStore& SettingsStore() {
  DLSSNR_PROFILE_CTL_L2("SettingsStore");
  static ctl::ValueStore store = [] {
    static ctl::Schema schema;
    RegisterAllSchemas(schema);
    schema.Build(nullptr);
    return ctl::ValueStore(schema);
  }();
  return store;
}

}  // namespace feature
}  // namespace reshade_dlssnr

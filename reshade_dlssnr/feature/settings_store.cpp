#include "feature/settings_store.h"
#include <reshade.hpp>
#include "profiling/profile.h"

#include "feature/register_all.h"

namespace reshade_dlssnr {
namespace feature {

ctl::ValueStore& SettingsStore() {
  DLSSNR_PROFILE_CTL_L2("SettingsStore");
  static ctl::ValueStore store = [] {
    static ctl::Schema schema;
    RegisterAllSchemas(schema);
    std::string error;
    if (!schema.Build(&error)) {
      // A Build failure is a programming error: two features declared the
      // same section name, a setting name collided within a section, a hook
      // anchored on a missing setting, or a rule named an unknown setting.
      // The store still has to be created -- returning early would turn a
      // schema mistake into a null dereference at every call site -- but the
      // reason is logged so the mistake is diagnosable rather than silent.
      reshade::log::message(reshade::log::level::error, error.c_str());
    }
    return ctl::ValueStore(schema);
  }();
  return store;
}

}  // namespace feature
}  // namespace reshade_dlssnr

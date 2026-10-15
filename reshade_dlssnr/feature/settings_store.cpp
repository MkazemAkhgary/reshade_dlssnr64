#include "feature/settings_store.h"

#include "feature/register_all.h"

namespace reshade_dlssnr {
namespace feature {

ctl::ValueStore& SettingsStore() {
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

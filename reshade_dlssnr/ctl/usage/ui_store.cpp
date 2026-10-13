#include "ctl/usage/ui_store.h"

#include <string>

#include "ctl/usage/ui_schema.h"

namespace reshade_dlssnr {
namespace ctl {

ValueStore& Store() {
  static ValueStore store = [] {
    static Schema schema;
    BuildSchema(schema, nullptr);
    return ValueStore(schema);
  }();
  return store;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

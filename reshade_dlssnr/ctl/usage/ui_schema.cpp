#include "ctl/usage/ui_schema.h"

#include "feature/register_all.h"

namespace reshade_dlssnr {
namespace ctl {

bool BuildSchema(Schema& schema, std::string* error) {
  feature::RegisterAllSchemas(schema);
  return schema.Build(error);
}

}  // namespace ctl
}  // namespace reshade_dlssnr

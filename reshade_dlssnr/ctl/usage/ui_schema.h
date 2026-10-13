// Schema construction: freeze the features' declarations.
//
// This is deliberately the whole file. The declarations live in
// feature/<name>/schema.cpp, reachable through feature/register_all.cpp, and
// each feature owns its own section names in its own settings.h; nothing about
// any particular feature appears here.

#pragma once

#include <string>

#include "ctl/schema.h"

namespace reshade_dlssnr {
namespace ctl {

// Declare every feature (see feature/register_all.h) and freeze the schema.
// Returns false and fills `error` on a duplicate name or a hook anchor that
// does not resolve.
bool BuildSchema(Schema& schema, std::string* error);

}  // namespace ctl
}  // namespace reshade_dlssnr

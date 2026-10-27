#include "ctl/types.h"
#include "profiling/profile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

bool BoolFromSetting(const void* context, const ValueStore& store,
                   const InstanceKey& key, const std::string& section) {
  const char* name = static_cast<const char*>(context);
  if (name == nullptr) return true;
  const Value value = store.Read(section, key, name);
  switch (value.kind) {
    case Value::Kind::Bool: return value.as_bool;
    case Value::Kind::Int:
    case Value::Kind::Enum: return value.as_int != 0;
    case Value::Kind::Float: return value.as_float != 0.0f;
    case Value::Kind::String: return !value.as_string.empty();
    case Value::Kind::IntList: return !value.as_int_list.empty();
  }
  return false;
}

bool Value::operator==(const Value& o) const {
  DLSSNR_PROFILE_CTL_L3("Value::operator==");
  if (kind != o.kind) return false;
  switch (kind) {
    case Kind::Float:
      // Bit-exact on purpose: the override tracker wants "did the stored
      // value change", not "is it approximately the same".
      return as_float == o.as_float;
    case Kind::Int:
    case Kind::Enum:
      return as_int == o.as_int;
    case Kind::Bool:
      return as_bool == o.as_bool;
    case Kind::String:
      return as_string == o.as_string;
    case Kind::IntList:
      return as_int_list == o.as_int_list;
  }
  return false;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

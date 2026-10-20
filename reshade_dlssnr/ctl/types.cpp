#include "ctl/types.h"
#include "profiling/profile.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reshade_dlssnr {
namespace ctl {

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

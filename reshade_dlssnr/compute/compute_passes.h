// Compute pass registry. Add new HLSL passes here without touching the codec.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct ComputePassDef {
  const char* name;
  const char* hlsl_source;
  // Prepended to hlsl_source before compilation. Empty for passes that do not
  // need it. This is how the colour grade's forward and inverse share one
  // definition across the three passes that use it, rather than three copies
  // that can drift.
  const char* prelude;
};

extern const ComputePassDef kComputePasses[];
extern const uint32_t kComputePassCount;

const char* FindComputePassSource(const char* name);
const ComputePassDef* FindComputePassDef(const char* name);

}  // namespace reshade_dlssnr

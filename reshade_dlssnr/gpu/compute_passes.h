// Compute pass registry. Add new HLSL passes here without touching the codec.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct ComputePassDef {
  const char* name;
  const char* hlsl_source;
  // Optional first prelude. Empty for passes that do not need it.
  const char* prelude;
  // Optional second prelude, concatenated after `prelude` and before the
  // pass-specific shader source. This lets independent shared shader blocks
  // compose without copying one prelude into another.
  const char* prelude2;
};

extern const ComputePassDef kComputePasses[];
extern const uint32_t kComputePassCount;

const char* FindComputePassSource(const char* name);
const ComputePassDef* FindComputePassDef(const char* name);

}  // namespace reshade_dlssnr

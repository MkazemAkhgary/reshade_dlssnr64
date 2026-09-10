// Compute pass registry. Add new HLSL passes here without touching the codec.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct ComputePassDef {
  const char* name;
  const char* hlsl_source;
};

extern const ComputePassDef kComputePasses[];
extern const uint32_t kComputePassCount;

const char* FindComputePassSource(const char* name);

}  // namespace reshade_dlssnr

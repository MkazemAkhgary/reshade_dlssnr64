// Compute pass registry. Each pass names its shader root relative to shaders/.
#pragma once

#include <cstddef>
#include <cstdint>

namespace reshade_dlssnr {

struct ComputePassDef {
  const char* name;         // trace name, matches the pass registry
  const char* root;         // path relative to shaders/
  const unsigned char* bytecode;
  std::size_t bytecode_size;
};

extern const ComputePassDef kComputePasses[];
extern const uint32_t kComputePassCount;

const ComputePassDef* FindComputePassDef(const char* name);

}  // namespace reshade_dlssnr

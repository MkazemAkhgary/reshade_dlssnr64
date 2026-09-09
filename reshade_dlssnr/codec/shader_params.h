// Dynamic shader parameter definitions for experimental effects.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct ShaderParamDef {
  const char* name;
  float min_value;
  float max_value;
  float default_value;
  bool is_checkbox;  // bool parameters rendered as checkbox
};

// Add experimental parameters here. They automatically appear in the UI,
// settings persistence, and shader cbuffer without further code changes.
constexpr ShaderParamDef kShaderParams[] = {
    {"LumaStrength",      0.0f, 1.0f, 1.0f,  false},
    {"ChromaStrength",    0.0f, 1.0f, 1.0f,  false},
    {"SoftCompression",   0.0f, 1.0f, 0.0f,  true },
    {"SoftMaxBrighten",   0.0f, 2.0f, 0.50f, false},
    {"SoftMaxDarken",     0.0f, 2.0f, 0.50f, false},
    {"SoftMaxChroma",     0.0f, 1.0f, 0.25f, false},
    {"SoftKnee",          0.0f, 4.0f, 1.00f, false},
};

constexpr uint32_t kShaderParamCount = sizeof(kShaderParams) / sizeof(kShaderParams[0]);

}  // namespace reshade_dlssnr

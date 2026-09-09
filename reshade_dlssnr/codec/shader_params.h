// Dynamic shader parameter definitions for experimental effects.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct ShaderParamDef {
  const char* name;
  const char* category;
  const char* tooltip;
  float min_value;
  float max_value;
  float default_value;
  bool is_checkbox;  // bool parameters rendered as checkbox
};

// Add experimental parameters here. They automatically appear in the UI
// grouped by category, persist to settings, and map to shader cbuffer
// without further code changes.
constexpr ShaderParamDef kShaderParams[] = {
    {"LumaStrength",   "Strength",  "Blend between original and edited luminance.",                         0.0f, 1.0f, 1.0f,  false},
    {"ChromaStrength", "Strength",  "Blend between original and edited chroma.",                            0.0f, 1.0f, 1.0f,  false},
    {"SoftCompression","Compression","Enable rational soft clamp of log residuals.",                        0.0f, 1.0f, 0.0f,  true },
    {"SoftMaxBrighten","Compression","Maximum allowed log-luma brightening.",                                0.0f, 2.0f, 0.50f, false},
    {"SoftMaxDarken",  "Compression","Maximum allowed log-luma darkening.",                                  0.0f, 2.0f, 0.50f, false},
    {"SoftMaxChroma",  "Compression","Maximum allowed log-chroma change per channel.",                       0.0f, 1.0f, 0.25f, false},
    {"SoftKnee",       "Compression","Controls how early compression starts (linear range).",                0.0f, 4.0f, 1.00f, false},
};

constexpr uint32_t kShaderParamCount = sizeof(kShaderParams) / sizeof(kShaderParams[0]);

}  // namespace reshade_dlssnr

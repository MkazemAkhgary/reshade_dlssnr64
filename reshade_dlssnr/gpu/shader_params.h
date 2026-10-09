// Dynamic shader parameter definitions for optional effects.
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
  // Name of another shader parameter this one depends on. When the named
  // parameter is off (checkbox value < 0.5), this control is disabled in
  // the UI (greyed, still visible). nullptr = always enabled.
  const char* depends_on;
};

// Parameters added here appear in the categorized UI, persist to settings,
// and map to the shader cbuffer automatically.
constexpr ShaderParamDef kShaderParams[] = {
    {"LumaStrength",   "Strength",   "Blend between original and edited luminance.",          0.0f, 1.0f, 1.00f, false, nullptr},
    {"ChromaStrength", "Strength",   "Blend between original and edited chroma.",             0.0f, 1.0f, 1.00f, false, nullptr},
    {"SoftCompression","Compression","Enable rational soft clamp of log residuals.",          0.0f, 1.0f, 0.00f, true,  nullptr},
    {"SoftMaxBrighten","Compression","Maximum allowed log-luma brightening.",                 0.0f, 2.0f, 0.50f, false, "SoftCompression"},
    {"SoftMaxDarken",  "Compression","Maximum allowed log-luma darkening.",                   0.0f, 2.0f, 0.50f, false, "SoftCompression"},
    {"SoftMaxChroma",  "Compression","Maximum allowed log-chroma change per channel.",        0.0f, 2.0f, 0.50f, false, "SoftCompression"},
    {"SoftKnee",       "Compression","Controls how early compression starts (linear range).", 0.0f, 4.0f, 1.00f, false, "SoftCompression"},
};

constexpr uint32_t kShaderParamCount = sizeof(kShaderParams) / sizeof(kShaderParams[0]);

}  // namespace reshade_dlssnr

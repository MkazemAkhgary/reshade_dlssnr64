#ifndef DLSSNR_COLOR_CONSTANTS_H
#define DLSSNR_COLOR_CONSTANTS_H

// Shared colorimetry constants. This file is intentionally valid as both C++
// and HLSL so format/reference-white assumptions have a single source of truth.
#ifdef __cplusplus
namespace reshade_dlssnr::color_constants {
inline constexpr float kScRgbReferenceWhiteNits = 80.0f;
inline constexpr float kSdrReferenceWhiteNits = 100.0f;
inline constexpr float kPqPeakNits = 10000.0f;
}  // namespace reshade_dlssnr::color_constants
#else
static const float kScRgbReferenceWhiteNits = 80.0;
static const float kSdrReferenceWhiteNits = 100.0;
static const float kPqPeakNits = 10000.0;
#endif

#endif

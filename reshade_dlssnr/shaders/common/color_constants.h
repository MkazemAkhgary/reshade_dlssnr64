#ifndef DLSSNR_COLOR_CONSTANTS_H
#define DLSSNR_COLOR_CONSTANTS_H

// Shared colorimetry constants. This file is intentionally valid as both C++
// and HLSL so format/reference-white assumptions have a single source of truth.
#ifdef __cplusplus
namespace reshade_dlssnr::color_constants {
inline constexpr float kScRgbReferenceWhiteNits = 80.0f;
inline constexpr float kSdrReferenceWhiteNits = 100.0f;
inline constexpr float kPqPeakNits = 10000.0f;
inline constexpr float kModelGamutCompressionStart = 0.90f;
inline constexpr float kModelGamutCompressionWhite = 0.945f;
inline constexpr float kModelGamutReferenceMappedWhite = 0.9975f;
inline constexpr float kModelGamutHighlightDesaturationStart = 1.0f;
inline constexpr float kModelHighlightDesaturation = 0.65f;
inline constexpr float kModelGamutReferenceLinear = 1.5f;
}  // namespace reshade_dlssnr::color_constants
#else
static const float kScRgbReferenceWhiteNits = 80.0;
static const float kSdrReferenceWhiteNits = 100.0;
static const float kPqPeakNits = 10000.0;
static const float kModelGamutCompressionStart = 0.90;
static const float kModelGamutCompressionWhite = 0.945;
static const float kModelGamutReferenceMappedWhite = 0.9975;
static const float kModelGamutHighlightDesaturationStart = 1.0;
static const float kModelHighlightDesaturation = 0.65;
static const float kModelGamutReferenceLinear = 1.5;
#endif

#endif

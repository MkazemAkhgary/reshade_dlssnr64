#pragma once

// Shared colorimetry constants. This file is intentionally valid as both C++
// and HLSL so format/reference-white assumptions have a single source of truth.
#ifdef __cplusplus
namespace reshade_dlssnr::color_constants {
inline constexpr float kScRgbReferenceWhiteNits = 80.0f;
inline constexpr float kSdrReferenceWhiteNits = 100.0f;
inline constexpr float kPqPeakNits = 10000.0f;
inline constexpr float kModelGamutCompressionStart = 0.90f;
inline constexpr float kModelGamutCompressionWhite = 0.945f;
// Model-facing highlight values map into a finite sub-white range. The source
// HDR magnitude remains available in the packed gain multiplier.
inline constexpr float kModelGamutReferenceMappedWhite = 0.9975f;
inline constexpr float kModelGamutExtendedMappedWhite = 0.9995f;
inline constexpr float kModelGamutHighlightDesaturationStart = 1.0f;
inline constexpr float kModelHighlightDesaturation = 0.65f;
inline constexpr float kModelGamutReferenceLinear = 16.0f;
inline constexpr float kModelGamutExtendedLinear = 65504.0f;
}  // namespace reshade_dlssnr::color_constants
#else
static const float kScRgbReferenceWhiteNits = 80.0;
static const float kSdrReferenceWhiteNits = 100.0;
static const float kPqPeakNits = 10000.0;
static const float kModelGamutCompressionStart = 0.90;
static const float kModelGamutCompressionWhite = 0.945;
static const float kModelGamutReferenceMappedWhite = 0.9975;
static const float kModelGamutExtendedMappedWhite = 0.9995;
static const float kModelGamutHighlightDesaturationStart = 1.0;
static const float kModelHighlightDesaturation = 0.65;
static const float kModelGamutReferenceLinear = 16.0;
static const float kModelGamutExtendedLinear = 65504.0;
#endif

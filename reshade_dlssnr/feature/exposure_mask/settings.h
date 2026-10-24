// Exposure Mask section and setting names. A leaf header: names only.

#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace exposure_mask {

inline constexpr const char* kSection = "Exposure Mask";
inline constexpr const char* kEnable = "enable";
inline constexpr const char* kEditStrength = "edit_strength";
inline constexpr const char* kSmoothing = "mask_smoothing";
inline constexpr const char* kResponse = "mask_response";
inline constexpr const char* kLumaThreshold = "luma_threshold";
inline constexpr const char* kLumaRelative = "luma_relative";
inline constexpr const char* kChromaThreshold = "chroma_threshold";

}  // namespace exposure_mask
}  // namespace feature
}  // namespace reshade_dlssnr

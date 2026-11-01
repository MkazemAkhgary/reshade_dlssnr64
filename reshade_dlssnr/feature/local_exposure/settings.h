// Local Exposure section and setting names.
//
// A leaf header: names only, no includes, so the schema, the view and the
// pass-keyed section list agree on the spelling without pulling in the
// runtime's LocalExposureSettings struct.
#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace local_exposure {

inline constexpr const char* kSection = "Local Exposure";

inline constexpr const char* kEnabled = "enabled";
inline constexpr const char* kStrength = "strength";
inline constexpr const char* kMaxBrighten = "max_brighten";
inline constexpr const char* kMaxDarken = "max_darken";
inline constexpr const char* kMidtone = "midtone";
inline constexpr const char* kShadowStrength = "shadow_strength";
inline constexpr const char* kHighlightStrength = "highlight_strength";
inline constexpr const char* kShadowRange = "shadow_range";
inline constexpr const char* kHighlightRange = "highlight_range";
inline constexpr const char* kHighlightProtection = "highlight_protection";
inline constexpr const char* kShadowProtection = "shadow_protection";
inline constexpr const char* kDetailRetention = "detail_retention";
inline constexpr const char* kEdgeProtection = "edge_protection";
inline constexpr const char* kRadius = "radius";
inline constexpr const char* kSamples = "samples";
inline constexpr const char* kRestoreEnabled = "restore_exposure";
inline constexpr const char* kRestoreMode = "restore_mode";
inline constexpr const char* kRestoreStrength = "restore_strength";

}  // namespace local_exposure
}  // namespace feature
}  // namespace reshade_dlssnr

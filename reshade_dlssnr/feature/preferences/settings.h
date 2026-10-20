// Preferences: the three global toggles.
//
// The smallest complete feature. Everything about it -- the section, its
// settings, their metadata and defaults -- is declared in RegisterSchema()
// below. Nothing about this feature appears anywhere else in the program.

#pragma once

#include <cstdint>

namespace reshade_dlssnr {
class ValueStore;

namespace feature {
namespace preferences {

inline constexpr const char* kSection = "Preferences";

// The setting names, so the one other place that must agree with the schema
// (the code that reads a preference back) can name them without a literal.
inline constexpr const char* kShowTooltips = "show_tooltips";
inline constexpr const char* kCloneResources = "clone_resources";
inline constexpr const char* kInvertFallbackDepth = "invert_fallback_depth";

inline constexpr int64_t kKey = 0;  // single instance

}  // namespace preferences
}  // namespace feature
}  // namespace reshade_dlssnr

// Debug view: one enum selecting which internal buffer replaces the image.
//
// Its item list comes from a runtime table (debug/debug_view.h). Rather than
// declaring the enum empty and filling it in after the schema is frozen, the
// setting takes an item provider at declaration time, so everything it needs
// is present the moment it is declared.

#pragma once

#include <cstdint>

namespace reshade_dlssnr {
namespace feature {
namespace debug_log {

inline constexpr const char* kSection = "Debug";
inline constexpr const char* kDebugView = "debug_view";
inline constexpr int64_t kKey = 0;

}  // namespace debug_log
}  // namespace feature
}  // namespace reshade_dlssnr

// Shared project-wide compile-time limits.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

constexpr uint32_t kMaxPasses = 8;

// Minimum vertical resolution, in pixels, for the model working scale and
// the temporal cheap-model scale. Very low resolutions leave temporal
// reprojection without usable history; the floor scales both dimensions
// up proportionally so the frame's aspect ratio is preserved.
constexpr uint32_t kMinWorkingResolutionHeight = 180;

}  // namespace reshade_dlssnr

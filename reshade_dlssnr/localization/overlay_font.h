// ReShade's font has no CJK glyphs unless its own Language is CJK.
#pragma once

namespace reshade_dlssnr {
namespace localization {

// Re-adds the source after an atlas rebuild.
void EnsureOverlayFont();

}  // namespace localization
}  // namespace reshade_dlssnr

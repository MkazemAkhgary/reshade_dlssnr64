// ReShade overlay entry point and all ImGui drawing.
#pragma once

#include <reshade.hpp>

namespace reshade_dlssnr {

void DrawOverlay(reshade::api::effect_runtime* runtime);

}  // namespace reshade_dlssnr

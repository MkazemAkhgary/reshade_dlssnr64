// Pass settings persistence: the per-pass values and inheritance flags.
#pragma once

#include <cstdint>

#include "state/settings.h"

namespace reshade_dlssnr {

void LoadPassSettings(uint32_t index, PassSettings& pass);
void SavePassSettings(uint32_t index, const PassSettings& pass);

}  // namespace reshade_dlssnr

// Pass settings persistence: the per-pass values and inheritance flags.
#pragma once

#include <cstdint>
#include <string>

#include "state/settings.h"

namespace reshade_dlssnr {

void LoadPassSettings(uint32_t index, PassSettings& pass, const std::string& key_prefix = {});
void SavePassSettings(uint32_t index, const PassSettings& pass, const std::string& key_prefix = {});

}  // namespace reshade_dlssnr

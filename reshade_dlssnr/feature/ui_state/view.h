// Accessors for the globally persisted overlay UI-state settings.
#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace ui_state {

bool Read(const char* setting);
bool Write(const char* setting, bool value);

}  // namespace ui_state
}  // namespace feature
}  // namespace reshade_dlssnr

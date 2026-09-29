// Section drawers used by DrawOverlay. Each lives in its own .cpp.
#pragma once

namespace reshade_dlssnr {

struct Settings;

void DrawSharedDebugView(Settings& settings, bool& changed);
bool AnyPassUsesDlssOutput(const Settings& settings);
void DrawStatusSection(Settings& settings, bool& changed);

}  // namespace reshade_dlssnr

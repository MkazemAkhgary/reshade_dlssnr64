// Section drawers used by DrawOverlay. Each lives in its own .cpp.
#pragma once

namespace reshade_dlssnr {

struct Settings;
struct GradeSettings;

void DrawSharedDebugView(Settings& settings, bool& changed);
bool AnyPassUsesDlssOutput(const Settings& settings);
void DrawStatusSection(Settings& settings, bool& changed);

void DrawGradeSection(const char* id,
                      const char* label,
                      const char* tooltip,
                      GradeSettings& grade,
                      bool& changed);

}  // namespace reshade_dlssnr

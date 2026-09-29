// Section drawers used by DrawOverlay. Each lives in its own .cpp.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct Settings;
struct GradeSettings;

// Set by the pass-body move buttons, applied at the top of the next frame by
// DrawOverlay. Applied out of line because the buttons are drawn inside the
// tab bar loop, and permuting the array mid-loop leaves the current tab's
// body half-drawn for one frame.
enum class PendingMove { None, Left, Right };
extern PendingMove g_pending_move;
extern int g_pending_move_index;

void DrawSharedDebugView(Settings& settings, bool& changed);
bool AnyPassUsesDlssOutput(const Settings& settings);
void DrawStatusSection(Settings& settings, bool& changed);

void DrawGradeSection(const char* id,
                      const char* label,
                      const char* tooltip,
                      GradeSettings& grade,
                      bool& changed);

// `hdr_relevant` gates the section: HDR only affects passes at DLSS Output.
void DrawHdrSection(Settings& settings, bool hdr_relevant, bool& changed);

void DrawSkinDetectSection(Settings& settings, bool& changed);

void DrawPassSettingsBody(uint32_t pass_index, Settings& settings,
                          bool& changed);

}  // namespace reshade_dlssnr

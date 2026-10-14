// Section drawers used by DrawOverlay. Each lives in its own .cpp.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct Settings;

// Set by the pass-body move buttons, applied at the top of the next frame by
// DrawOverlay. Applied out of line because the buttons are drawn inside the
// tab bar loop, and permuting the array mid-loop leaves the current tab's
// body half-drawn for one frame.
enum class PendingMove { None, Left, Right };
extern PendingMove g_pending_move;
extern int g_pending_move_index;

enum class PendingPassAction { None, Add, Remove };
extern PendingPassAction g_pending_pass_action;

// Pass-management toolbar drawn on the tab-bar row, to the right of the tabs.
// It acts on the selected pass and lives outside the tab items so tab
// hit-testing can never swallow the controls. `PassToolbarWidth()` reports the
// horizontal space the caller must reserve before laying out the tab bar.
void DrawPassToolbar(Settings& settings, bool& changed);
float PassToolbarWidth();

bool AnyPassUsesHdrInjection(const Settings& settings);
void DrawStatusSection(Settings& settings, bool& changed);

// Pass bookkeeping. The pass body itself is declared in the ctl schema; these
// are the array-plumbing operations the overlay still owns.
void AddPass(Settings& settings, bool& changed);
void RemoveActivePass(Settings& settings, bool& changed);

}  // namespace reshade_dlssnr

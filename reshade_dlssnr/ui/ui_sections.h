// Section drawers used by DrawOverlay. Each lives in its own .cpp.
#pragma once

#include <cstdint>

namespace reshade_dlssnr {

struct Settings;

// Set by the pass-management toolbar, applied at the top of the next frame
// by DrawOverlay so pass-order changes cannot invalidate the current frame's
// tab layout.
enum class PendingMove { None, Left, Right };
extern PendingMove g_pending_move;
extern int g_pending_move_index;

enum class PendingPassAction { None, Add, Remove };
extern PendingPassAction g_pending_pass_action;

// Pass-management toolbar drawn on its own row after the wrapped pass tabs.
// Actions are deferred until the next frame so pass-order changes cannot
// invalidate the active frame's tab layout.
void DrawPassToolbar(Settings& settings, bool& changed);

bool AnyPassUsesHdrInjection(const Settings& settings);
void DrawStatusSection(Settings& settings, bool& changed);

// Pass bookkeeping. The pass body itself is declared in the ctl schema; these
// are the array-plumbing operations the overlay still owns.
void AddPass(Settings& settings, bool& changed);
void RemoveActivePass(Settings& settings, bool& changed);

}  // namespace reshade_dlssnr

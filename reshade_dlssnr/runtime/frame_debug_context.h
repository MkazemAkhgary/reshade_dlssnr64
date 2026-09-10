// Per-frame debug capture. The active pass records a snapshot of its
// pipeline resources here; the Present event renders that snapshot to the
// swapchain backbuffer. This keeps the debug display entirely out of the
// game's rendering path — no debug pixels ever touch the game's resources.
#pragma once

#include "debug_view.h"

namespace reshade_dlssnr {

struct FrameDebugContext {
  DebugSnapshot snapshot;
  int debug_view = 0;    // 0 = off, otherwise kDebugViews[debug_view-1]
  bool valid = false;

  void Reset() {
    snapshot = {};
    debug_view = 0;
    valid = false;
  }
};

}  // namespace reshade_dlssnr

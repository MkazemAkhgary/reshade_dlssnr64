// A snapshot of the addon's current health: what is running, what has
// failed, and why. Distinct from the log: the log is a historical record
// of events, the status is "what is true right now". The controller
// refreshes the snapshot at the end of every Present or Post-DLSS pass;
// the UI reads it.
//
// The snapshot is copyable and self-contained — no pointers into live
// state, no lifetimes to manage.
#pragma once

#include <cstdint>

#include "state/settings.h"

namespace reshade_dlssnr {

constexpr uint32_t kStatusTextLength = 160;

// Per-pass status. Split by ownership:
//   - `enabled` is setting-owned. Populated by AddonController::RefreshStatus
//     from the current settings; the session itself has no access to them.
//   - The rest are runtime-owned, populated by PassSession::GetStatus.
//
// The pass's injection stage (DLSS Output / Backbuffer) is deliberately not
// duplicated here — it is a setting, and the UI reads it directly from
// Settings when formatting the row.
struct PassStatus {
  bool enabled = false;                          // user toggle (setting)
  bool running = false;                          // ran the last frame
  bool failed = false;                           // failed at some point
  bool has_feature = false;                      // NGX feature handle exists
  char reason[kStatusTextLength] = {};           // failure reason, or ""
};

struct StatusSnapshot {
  // Master switch.
  bool enabled = false;

  // NGX runtime (nvngx_dlssnr.dll): loaded, patched, and initialized.
  bool runtime_initialized = false;
  char runtime_reason[kStatusTextLength] = {};

  // DLSS guides: depth and motion captured from the game's DLSS eval.
  bool guides_valid = false;

  // Post-DLSS path blocked. Set when the NGX hook could not run the DLSS
  // Output passes because the D3D12 command list hooks are not installed,
  // so the game's command list state could not be put back afterwards.
  // Cleared once the condition goes away.
  bool post_dlss_blocked = false;
  char post_dlss_block_reason[kStatusTextLength] = {};

  // HDR stage (shared across passes; only used by Post-DLSS passes).
  bool hdr_ready = false;        // has consumed at least one measurement
  float hdr_exposure = 1.0f;
  float hdr_scene_white = 1.0f;
  bool gain_map_ready = false;
  float gain_map_min_log2 = 0.0f;
  float gain_map_max_log2 = 0.0f;
  uint32_t gain_map_sdr_excursion_pixels = 0;
  uint32_t gain_map_out_of_range_pixels = 0;
  char hdr_reason[kStatusTextLength] = {};

  // Per-pass state.
  uint32_t pass_count = 0;
  PassStatus passes[kMaxPasses];
};

}  // namespace reshade_dlssnr

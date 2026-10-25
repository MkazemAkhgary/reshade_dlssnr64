// A snapshot of the addon's current health: what is running, what has
// failed, and why. Distinct from the log: the log is a historical record
// of events, the status is "what is true right now". The controller
// refreshes the snapshot at the end of every Present or Post-DLSS pass;
// the UI reads it.
//
// The snapshot is copyable and self-contained - no pointers into live
// state, no lifetimes to manage.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "feature/pass/ui.h"
#include "state/settings.h"

namespace reshade_dlssnr {

constexpr uint32_t kStatusTextLength = 160;

// One GPU allocation owned by a pass. Captured from a live PassResources
// every frame, so it reflects the current allocation rather than a lifetime
// high-water mark. A texture is identified by width * height * bpp; a buffer
// has height == 0 and its width is the byte size.
constexpr uint32_t kMaxPassResourceEntries = 32;

struct ResourceInfoEntry {
  char name[32] = {};
  uint32_t width = 0;
  uint32_t height = 0;
  // DXGI_FORMAT for textures; UNKNOWN for buffers. Kept numeric so this
  // header does not pull in d3d12.h for its consumers.
  uint32_t format = 0;
  uint64_t bytes = 0;
};

struct PassResourceStats {
  ResourceInfoEntry entries[kMaxPassResourceEntries];
  uint32_t count = 0;
  uint64_t total_bytes = 0;
};

// Per-pass status. Split by ownership:
//   - `enabled` is setting-owned. Populated by AddonController::RefreshStatus
//     from the current settings; the session itself has no access to them.
//   - The rest are runtime-owned, populated by PassSession::GetStatus.
//
// The pass's injection stage (Pre-DLSS / Post-DLSS / Backbuffer) is deliberately not
// duplicated here - it is a setting, and the UI reads it directly from
// Settings when formatting the row.
struct PassStatus {
  bool enabled = false;                          // user toggle (setting)
  bool running = false;                          // ran the last frame
  bool failed = false;                           // failed at some point
  bool has_feature = false;                      // NGX feature handle exists
  char reason[kStatusTextLength] = {};           // failure reason, or ""
  // Current allocations from this pass's PassResources. Populated by
  // PassSession::GetStatus; empty for a pass whose session has not been
  // created yet.
  PassResourceStats resources;
};

struct PassStatusEntry {
  feature::pass::PassId id = 0;
  PassStatus status;
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

  // Pre-DLSS path blocked for this game. Ray Reconstruction is the current
  // case: its Color arrives through a subrect the compact-color path cannot
  // rewrite safely, so the stage is skipped rather than risking the GPU.
  bool pre_dlss_blocked = false;
  char pre_dlss_block_reason[kStatusTextLength] = {};

  // HDR stage (shared across Pre-DLSS/Post-DLSS game-resource passes).
  bool hdr_ready = false;        // has consumed at least one measurement
  float hdr_exposure = 1.0f;
  float hdr_scene_white = 1.0f;
  bool gain_map_ready = false;
  float gain_map_min_log2 = 0.0f;
  float gain_map_max_log2 = 0.0f;
  uint32_t gain_map_sdr_excursion_pixels = 0;
  uint32_t gain_map_out_of_range_pixels = 0;
  char hdr_reason[kStatusTextLength] = {};

  // One entry per live pass, in pass_order order. Each entry identifies its
  // own pass; consumers correlate it with settings or sessions by id, never
  // by assuming the vector's position has identity semantics.
  std::vector<PassStatusEntry> passes;

  // Sum of every pass's PassResources::total_bytes at the last refresh. Does
  // not include the shared compute engine's descriptor heap, constants
  // buffer, or the controller's release command context; those are addon-wide
  // rather than per-pass.
  uint64_t total_vram_bytes = 0;

  // Resources waiting for a queue-ordered fence before release. Nonzero means
  // a recent allocation change retired something this frame; zero is the
  // steady state.
  uint32_t retired_game_resources = 0;
  uint32_t retired_addon_resources = 0;

  // Clears all fields without releasing vector capacity. Once steady state is
  // reached, RefreshStatus can rebuild the snapshot without allocating.
  void Reset() {
    enabled = false;
    runtime_initialized = false;
    std::fill_n(runtime_reason, kStatusTextLength, '\0');
    guides_valid = false;
    post_dlss_blocked = false;
    std::fill_n(post_dlss_block_reason, kStatusTextLength, '\0');
    pre_dlss_blocked = false;
    std::fill_n(pre_dlss_block_reason, kStatusTextLength, '\0');

    hdr_ready = false;
    hdr_exposure = 0.0f;
    hdr_scene_white = 0.0f;
    gain_map_ready = false;
    gain_map_min_log2 = 0.0f;
    gain_map_max_log2 = 0.0f;
    gain_map_sdr_excursion_pixels = 0;
    gain_map_out_of_range_pixels = 0;
    std::fill_n(hdr_reason, kStatusTextLength, '\0');

    passes.clear();
    total_vram_bytes = 0;
    retired_game_resources = 0;
    retired_addon_resources = 0;
  }
};

}  // namespace reshade_dlssnr

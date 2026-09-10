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

#include "settings.h"

namespace reshade_dlssnr {

constexpr uint32_t kStatusTextLength = 160;

struct PassStatus {
  bool enabled = false;                          // user toggle
  bool running = false;                          // ran this frame
  bool failed = false;                           // failed at some point
  InjectionStage stage = InjectionStage::Present;
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

  // HDR stage (shared across passes; only used by Post-DLSS passes).
  bool hdr_enabled = false;
  bool hdr_ready = false;        // has consumed at least one measurement
  float hdr_exposure = 1.0f;
  float hdr_white_point = 1.0f;
  char hdr_reason[kStatusTextLength] = {};

  // Per-pass state.
  uint32_t pass_count = 0;
  PassStatus passes[kMaxPasses];
};

}  // namespace reshade_dlssnr

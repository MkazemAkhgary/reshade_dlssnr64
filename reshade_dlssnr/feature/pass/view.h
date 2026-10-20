// Pass view: read one pass instance out of the store as a PassSnapshot.
//
// The store is the source of truth for settings; the runtime wants a plain
// value. This is the one place that translates between them for the Pass
// section, so the setting names appear once and the runtime never spells one.
//
// A view reads; it does not write. What a pass owns is what it runs at, so a
// value read here is the value the row draws.
//
// The two model-boundary grades are separate sections and are read by
// feature/grade/view.cpp; this view covers the Pass section itself.

#pragma once

#include <string>

#include "compose/settings.h"

namespace reshade_dlssnr {
namespace ctl {
class ValueStore;
class InstanceKey;
}  // namespace ctl

namespace feature {
namespace pass {

// Everything the runtime reads off one pass, in one plain value.
//
// It is the Pass section's settings, resolved. `injection_stage` is the
// target as an InjectionStage; the enum lives in state/settings.h, so the
// view exposes the raw int and the runtime narrows it at the boundary it
// already owns.
struct PassSnapshot {
  // Identity / targeting.
  bool enabled = true;
  int target = 2;  // InjectionStage; 2 == Backbuffer
  // Display name for the pass tab. Empty means use the positional default
  // ("Pass N"). Part of the snapshot because the tab bar draws it, and the tab
  // bar must not reach past the view to spell the setting name itself.
  std::string custom_name;

  // Model group.
  int preset = 0;
  float working_scale = 1.0f;
  float intensity = 1.0f;
  // 0=Off, 1=Adaptive, 2=Every 2, 3=Every 4.
  int temporal_mode = 0;
  bool synthetic_motion_guides = false;
  float temporal_photometric_abs_threshold = 0.06f;
  float temporal_photometric_rel_threshold = 0.10f;
  float temporal_depth_abs_threshold = 0.005f;
  float temporal_depth_rel_threshold = 0.03f;
  float temporal_max_displacement = 40.0f;
  float temporal_max_invalid_percent = 5.0f;
  bool temporal_fp16_history = false;
  float local_tone = 1.0f;
  float local_structure = 1.0f;
  float skin_structure = 1.0f;
  bool auto_mask = true;
  bool ui_correction = true;

  // Strength group.
  StrengthSettings strength;

  // Advanced group.
  SoftCompressionSettings soft_compression;
  ResidualSettings residual;
};

// Read the Pass section for `key`.
PassSnapshot Read(const ctl::ValueStore& store, const ctl::InstanceKey& key);

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

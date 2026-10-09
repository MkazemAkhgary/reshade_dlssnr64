// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

#include "compose/settings.h"
#include "grade/settings.h"
#include "hdr/settings.h"
#include "skin/settings.h"
#include "project_constants.h"

namespace reshade_dlssnr {

// Storage capacity for PassSettings::params and ::inherit_params. Kept
// larger than the current table on purpose; settings.cpp static_asserts
// the two stay in step.
constexpr uint32_t kShaderParamCapacity = 32;

enum class InjectionStage : int {
  PostDLSS = 0,
  Present = 1,
};



struct PassSettings {
  int preset = 1;

  float local_tone = 1.0f;
  bool ui_correction = true;

  float local_structure = 1.0f;
  bool auto_mask = true;
  float skin_structure = 1.0f;

  float intensity = 1.0f;

  float working_scale = 1.0f;

  // Where this pass runs. Constraint: if any earlier pass runs at Present,
  // this pass must also run at Present. Enforced at load time and in the UI.
  InjectionStage injection_stage = InjectionStage::Present;

  // Grades at this pass's two model boundaries. Not inheritable — carrying
  // one into the next pass would apply it twice.
  GradeSettings grade_input;
  GradeSettings grade_output;

  // How this pass's model edit is carried back. Not inheritable.
  ResidualSettings residual;

  // FSR RCAS on the EASU-upsampled working images. Not inheritable.
  bool rcas_enabled = false;
  float rcas_strength = 0.5f;  // [0, 1]

  // Only meaningful for pass index > 0; a true flag makes the slider use
  // the previous pass's resolved value.
  bool inherit_working_scale = false;
  bool inherit_intensity = false;
  bool inherit_local_tone = false;
  bool inherit_local_structure = false;
  bool inherit_skin_structure = false;
  bool inherit_params[kShaderParamCapacity] = {};

  // Dynamic shader parameter values, indexed by kShaderParams order.
  float params[kShaderParamCapacity] = {};

  // Display name for the pass tab. Purely cosmetic; empty means use the
  // positional default ("Pass N").
  char custom_name[64] = {};

};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  // When true, hover tooltips are drawn in the overlay. Exposed next to
  // the master switch as "Show Tooltips"; defaults to on.
  bool show_tooltips = true;

  // Debug view selection, shared across all passes. 0 = off; N (1-based)
  // selects kDebugViews[N-1] in runtime/debug_view.h. The active pass
  // renders the selected view; switching tabs keeps the selection.
  int debug_view = 0;

  bool pass_enabled[kMaxPasses] = {true, false, false};
  PassSettings passes[kMaxPasses];

  // UI state: which pass tab is currently open. Determines whose debug view
  // is rendered. Transient (not persisted).
  int active_pass = 0;

  HdrSettings hdr;
  SkinSettings skin;

  // ---------------------------------------------------------------------
  // Final backbuffer grade. Runs once per frame, after every pass, on the
  // swapchain backbuffer itself — the last thing before the game presents.
  // Global rather than per-pass because there is one backbuffer.
  // ---------------------------------------------------------------------
  GradeSettings grade_backbuffer;

  // UI state: which collapsible sections are open. Persisted, and shared
  // across passes — collapsing a section in one tab collapses it in every
  // tab, so the panel keeps a single, consistent shape. `ui_category_open`
  // is indexed by the order categories appear in kShaderParams (0, 1, ...);
  // entries past the current category count are unused. Model and each
  // category default to open.
  bool ui_model_open = true;
  bool ui_category_open[16] = {
      true, true, true, true, true, true, true, true,
      true, true, true, true, true, true, true, true};
  bool ui_residual_open = true;
  bool ui_rcas_open = true;
  bool ui_status_open = true;
  bool ui_hdr_open = true;
  bool ui_grade_open = true;
  bool ui_hdr_diagnostics_open = false;
  bool ui_skin_detect_open = true;
  bool ui_skin_detect_model_open = true;
  bool ui_skin_detect_mask_open = true;
};

Settings& GetSettings();

// Resolved pass settings with inheritance applied. This is derived state,
// kept in sync by LoadSettings and SaveSettings. The runtime reads it
// directly, so no per-frame resolution is ever needed. Index 0 is always
// the raw settings (there is nothing to inherit from).
//
// Inheritance is a UI / settings-change concern: the resolved array is
// recomputed whenever the settings layer changes, and everyone else just
// reads the result.
const PassSettings& GetResolvedPass(uint32_t index);

void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

#include "compose/settings.h"
#include "grade/settings.h"
#include "hdr/settings.h"
#include "importance_mask/settings.h"
#include "project_constants.h"

namespace reshade_dlssnr {

enum class InjectionStage : int {
  PreDLSS = 0,
  PostDLSS = 1,
  Backbuffer = 2,
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

  // Independent target for this pass. Runtime groups passes in execution order:
  // Pre-DLSS, Post-DLSS, then Backbuffer; tab order is preserved within each group.
  InjectionStage injection_stage = InjectionStage::Backbuffer;

  // Grades at this pass's two model boundaries. Not inheritable — carrying
  // one into the next pass would apply it twice.
  GradeSettings grade_input;
  GradeSettings grade_output;

  // How this pass's model edit is carried back. Not inheritable.
  ResidualSettings residual;

  // Strength section: luma/chroma blend of the model edit. Inheritable.
  StrengthSettings strength;
  bool inherit_luma_strength = false;
  bool inherit_chroma_strength = false;

  // Soft compression of the residual delta. Sliders are inheritable; the
  // enabled toggle is per-pass.
  SoftCompressionSettings soft_compression;
  bool inherit_soft_max_brighten = false;
  bool inherit_soft_max_darken = false;
  bool inherit_soft_max_chroma = false;
  bool inherit_soft_knee = false;

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

  // Display name for the pass tab. Purely cosmetic; empty means use the
  // positional default ("Pass N").
  char custom_name[64] = {};

};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  // When true, hover tooltips are drawn in the overlay. Exposed in the
  // Preferences section; defaults to on.
  bool show_tooltips = true;

  // Shared advanced guide/output resource policy. Disabled uses game-owned
  // depth, motion, and post-DLSS output resources directly; enabled snapshots
  // current-evaluation resources into addon-owned clones for timing tests.
  bool clone_resources = false;

  // Debug view selection, shared across all passes. 0 = off; N (1-based)
  // selects kDebugViews[N-1] in runtime/debug_view.h. The active pass
  // renders the selected view; switching tabs keeps the selection.
  int debug_view = 0;

  uint32_t pass_count = 3;
  bool pass_enabled[kMaxPasses] = {true, false, false};
  PassSettings passes[kMaxPasses];

  // UI state: which pass tab is currently open. Determines whose debug view
  // is rendered. Transient (not persisted).
  int active_pass = 0;

  HdrSettings hdr;
  ImportanceMaskSettings importance_mask;

  // ---------------------------------------------------------------------
  // Final backbuffer grade. Runs once per frame, after every pass, on the
  // swapchain backbuffer itself — the last thing before the game presents.
  // Global rather than per-pass because there is one backbuffer.
  // ---------------------------------------------------------------------
  GradeSettings grade_backbuffer;

  // UI state: which collapsible sections are open. Persisted, and shared
  // across passes — collapsing a section in one tab collapses it in every
  // tab, so the panel keeps a single, consistent shape. Model, Strength,
  // and each sub-section default to open except Advanced.
  bool ui_model_open = true;
  bool ui_strength_open = true;
  bool ui_soft_compression_open = true;
  bool ui_pass_advanced_open = false;
  bool ui_residual_open = false;
  bool ui_rcas_open = false;
  bool ui_grade_input_open = false;
  bool ui_grade_output_open = false;
  bool ui_status_open = true;
  bool ui_preferences_open = false;
  bool ui_presets_open = false;
  bool ui_passes_open = true;
  bool ui_debug_open = false;
  bool ui_hdr_open = true;
  bool ui_grade_open = true;
  bool ui_hdr_diagnostics_open = false;
  bool ui_bounded_tuning_open = false;
  bool ui_importance_mask_open = true;
};

constexpr uint32_t kMaxSavedProfiles = 16;
constexpr uint32_t kSavedProfileNameCapacity = 64;

struct SavedProfileInfo {
  char name[kSavedProfileNameCapacity] = {};
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

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity);
bool SaveCurrentAsProfile(const char* name);
bool SaveCurrentToProfile(uint32_t index);
bool LoadSavedProfile(uint32_t index);
bool DeleteSavedProfile(uint32_t index);

}  // namespace reshade_dlssnr

// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

namespace reshade_dlssnr {

constexpr uint32_t kMaxPasses = 3;

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

  // Inheritance flags. Only meaningful for pass index > 0; the root pass
  // ignores them. A true flag makes the corresponding slider use the
  // resolved value from the previous pass instead of its own raw value.
  bool inherit_working_scale = false;
  bool inherit_intensity = false;
  bool inherit_local_tone = false;
  bool inherit_local_structure = false;
  bool inherit_skin_structure = false;
  bool inherit_params[16] = {};

  // Dynamic shader parameter values, indexed by kShaderParams order.
  float params[16] = {};
};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  // Debug view selection, shared across all passes. 0 = off; N (1-based)
  // selects kDebugViews[N-1] in runtime/debug_view.h. The active pass
  // renders the selected view; switching tabs keeps the selection.
  int debug_view = 0;

  // Pass 0 is always enabled; the remaining entries are user-toggleable.
  bool pass_enabled[kMaxPasses] = {true, false, false};
  PassSettings passes[kMaxPasses];

  // UI state: which pass tab is currently open. Determines whose debug view
  // is rendered. Transient (not persisted).
  int active_pass = 0;
};

Settings& GetSettings();

// Returns pass `index` with inherited values resolved from the chain of
// earlier passes. For index 0, returns the settings unchanged.
PassSettings ResolvePassSettings(uint32_t index);

void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

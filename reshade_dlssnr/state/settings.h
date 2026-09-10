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

  // Debug view selection for this pass. 0 = off; N (1-based) selects
  // kDebugViews[N-1] in runtime/debug_view.h.
  int debug_view = 0;

  // Dynamic shader parameter values, indexed by kShaderParams order.
  float params[16] = {};
};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  // Pass 0 is always enabled; the remaining entries are user-toggleable.
  bool pass_enabled[kMaxPasses] = {true, false, false};
  PassSettings passes[kMaxPasses];
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

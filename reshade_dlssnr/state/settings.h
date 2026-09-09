// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <reshade.hpp>

namespace reshade_dlssnr {

struct PassSettings {
  int preset = 1;

  float local_tone = 1.0f;
  bool ui_correction = true;

  float local_structure = 1.0f;
  bool auto_mask = true;
  float skin_structure = 1.0f;

  float intensity = 1.0f;

  float working_scale = 1.0f;

  // Dynamic shader parameter values, indexed by kShaderParams order.
  float params[16] = {};
};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;
  bool second_pass_enabled = false;
  bool third_pass_enabled = false;

  PassSettings pass1;
  PassSettings pass2;
  PassSettings pass3;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

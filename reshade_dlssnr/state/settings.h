// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <reshade.hpp>

namespace reshade_dlssnr {

struct PassSettings {
  int preset = 1;
  int style = 1;

  float local_tone = 1.0f;
  bool ui_correction = true;

  float local_structure = 1.0f;
  bool auto_mask = true;
  float skin_structure = 1.0f;

  float intensity = 1.0f;
  float luma_strength = 1.0f;
  float chroma_strength = 1.0f;

  float working_scale = 1.0f;
  float residual_suppression = 0.85f;
  float residual_threshold = 0.10f;
};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;
  bool second_pass_enabled = false;

  PassSettings pass1;
  PassSettings pass2;

  bool use_selected_pass_as_input = false;
  float ui_mask_threshold = 0.02f;
  float ui_mask_softness = 0.06f;

  int debug_view = 0;
  int compare_mode = 0;
  float compare_split = 0.5f;
  float compare_zoom = 1.0f;
  bool compare_swap = false;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

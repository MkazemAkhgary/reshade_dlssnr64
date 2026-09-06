#pragma once

#include <reshade.hpp>

namespace reshade_dlssnr {

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  float intensity = 0.50f;
  float chroma_strength = 1.0f;
  float luma_strength = 1.0f;
  float white_point_scale = 100.0f;
  float max_ratio = 4.0f;
  float working_scale = 1.0f;
  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;

  int preset = 1;
  int style = 1;
  float local_structure = 0.0f;
  float local_tone = 0.0f;
  float skin_structure = 0.0f;
  bool auto_mask = true;
  bool ui_correction = true;

  int debug_view = 0;
  int compare_mode = 0;
  float compare_split = 0.5f;
  float compare_zoom = 1.0f;
  bool compare_swap = false;

  bool passthrough_tone_mapped = false;
  bool use_selected_pass_as_input = false;
  float ui_mask_threshold = 0.02f;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

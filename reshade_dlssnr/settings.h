#pragma once

#include <reshade.hpp>

namespace reshade_dlssnr {

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  float intensity = 0.50f;
  float transfer_strength = 0.50f;
  float colour_strength = 0.50f;
  float white_point_scale = 100.0f;
  float max_ratio = 4.0f;
  float working_scale = 1.0f;
  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;

  unsigned int preset = 2u;
  unsigned int style = 1u;
  float local_structure = 0.0f;
  float local_tone = 0.0f;
  float skin_structure = 0.0f;
  bool auto_mask = true;

  unsigned int debug_view = 0u;
  unsigned int compare_mode = 0u;
  float compare_split = 0.5f;
  float compare_zoom = 1.0f;
  bool compare_swap = false;

  bool passthrough_tone_mapped = false;
};

Settings& GetSettings();
void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

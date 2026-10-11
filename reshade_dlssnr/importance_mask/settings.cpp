#include "importance_mask/settings.h"

#include <algorithm>

#include <reshade.hpp>

namespace reshade_dlssnr {

void LoadImportanceMaskSettings(ImportanceMaskSettings& settings) {
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Enable",
                            settings.enable);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.LumaThreshold",
                            settings.luma_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.LumaRelative",
                            settings.luma_relative);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.ChromaThreshold",
                            settings.chroma_threshold);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.EditStrength",
                            settings.edit_strength);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Smoothing",
                            settings.mask_smoothing);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Response",
                            settings.mask_response);

  settings.luma_threshold = std::clamp(settings.luma_threshold, 0.0f, 0.5f);
  settings.luma_relative = std::clamp(settings.luma_relative, 0.0f, 1.0f);
  settings.chroma_threshold = std::clamp(settings.chroma_threshold, 0.0f, 0.5f);
  settings.edit_strength = std::clamp(settings.edit_strength, 0.1f, 8.0f);
  settings.mask_smoothing = std::clamp(settings.mask_smoothing, 0.0f, 1.0f);
  settings.mask_response = std::clamp(settings.mask_response, 0.0f, 1.0f);
}

void SaveImportanceMaskSettings(const ImportanceMaskSettings& settings) {
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Enable",
                            settings.enable);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.LumaThreshold",
                            settings.luma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.LumaRelative",
                            settings.luma_relative);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.ChromaThreshold",
                            settings.chroma_threshold);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.EditStrength",
                            settings.edit_strength);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Smoothing",
                            settings.mask_smoothing);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", "ImportanceMask.Response",
                            settings.mask_response);
}

}  // namespace reshade_dlssnr

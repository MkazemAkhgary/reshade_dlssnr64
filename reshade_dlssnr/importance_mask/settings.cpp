#include "importance_mask/settings.h"

#include <algorithm>
#include <string>

#include <reshade.hpp>

namespace reshade_dlssnr {
namespace {
template <typename T>
void LoadValue(const std::string& root, const char* key, T& value) {
  const std::string full_key = root + key;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}
template <typename T>
void SaveValue(const std::string& root, const char* key, const T& value) {
  const std::string full_key = root + key;
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}
}  // namespace

void LoadImportanceMaskSettings(ImportanceMaskSettings& settings, const std::string& key_prefix) {
  LoadValue(key_prefix, "ImportanceMask.Enable", settings.enable);
  LoadValue(key_prefix, "ImportanceMask.LumaThreshold", settings.luma_threshold);
  LoadValue(key_prefix, "ImportanceMask.LumaRelative", settings.luma_relative);
  LoadValue(key_prefix, "ImportanceMask.ChromaThreshold", settings.chroma_threshold);
  LoadValue(key_prefix, "ImportanceMask.EditStrength", settings.edit_strength);
  LoadValue(key_prefix, "ImportanceMask.Smoothing", settings.mask_smoothing);
  LoadValue(key_prefix, "ImportanceMask.Response", settings.mask_response);

  settings.luma_threshold = std::clamp(settings.luma_threshold, 0.0f, 0.5f);
  settings.luma_relative = std::clamp(settings.luma_relative, 0.0f, 1.0f);
  settings.chroma_threshold = std::clamp(settings.chroma_threshold, 0.0f, 0.5f);
  settings.edit_strength = std::clamp(settings.edit_strength, 0.1f, 8.0f);
  settings.mask_smoothing = std::clamp(settings.mask_smoothing, 0.0f, 1.0f);
  settings.mask_response = std::clamp(settings.mask_response, 0.0f, 1.0f);
}

void SaveImportanceMaskSettings(const ImportanceMaskSettings& settings, const std::string& key_prefix) {
  SaveValue(key_prefix, "ImportanceMask.Enable", settings.enable);
  SaveValue(key_prefix, "ImportanceMask.LumaThreshold", settings.luma_threshold);
  SaveValue(key_prefix, "ImportanceMask.LumaRelative", settings.luma_relative);
  SaveValue(key_prefix, "ImportanceMask.ChromaThreshold", settings.chroma_threshold);
  SaveValue(key_prefix, "ImportanceMask.EditStrength", settings.edit_strength);
  SaveValue(key_prefix, "ImportanceMask.Smoothing", settings.mask_smoothing);
  SaveValue(key_prefix, "ImportanceMask.Response", settings.mask_response);
}

}  // namespace reshade_dlssnr

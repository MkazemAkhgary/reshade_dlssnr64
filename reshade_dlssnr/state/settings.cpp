#include "state/settings.h"
#include "hooks/dlss_guides.h"

#include <reshade.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#include "compose/settings.h"
#include "grade/settings.h"
#include "state/pass.h"
#include "state/trace.h"

namespace reshade_dlssnr {

namespace {
Settings g_settings;
PassSettings g_resolved[kMaxPasses];

void RecomputeResolved() {
  g_resolved[0] = g_settings.passes[0];
  for (uint32_t i = 1; i < kMaxPasses; ++i) {
    PassSettings r = g_settings.passes[i];
    const PassSettings& prev = g_resolved[i - 1];

    if (r.inherit_working_scale) r.working_scale = prev.working_scale;
    if (r.inherit_intensity) r.intensity = prev.intensity;
    if (r.inherit_local_tone) r.local_tone = prev.local_tone;
    if (r.inherit_local_structure) r.local_structure = prev.local_structure;
    if (r.inherit_skin_structure) r.skin_structure = prev.skin_structure;
    if (r.inherit_luma_strength) r.strength.luma_strength = prev.strength.luma_strength;
    if (r.inherit_chroma_strength) r.strength.chroma_strength = prev.strength.chroma_strength;
    if (r.inherit_soft_max_brighten) r.soft_compression.max_brighten = prev.soft_compression.max_brighten;
    if (r.inherit_soft_max_darken) r.soft_compression.max_darken = prev.soft_compression.max_darken;
    if (r.inherit_soft_max_chroma) r.soft_compression.max_chroma = prev.soft_compression.max_chroma;
    if (r.inherit_soft_knee) r.soft_compression.knee = prev.soft_compression.knee;
    g_resolved[i] = r;
  }
}

template <typename T>
void ReadValue(const std::string& root, const char* key, T& value) {
  const std::string full_key = root + key;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}

template <typename T>
void WriteValue(const std::string& root, const char* key, const T& value) {
  const std::string full_key = root + key;
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}

std::string PassEnabledKey(uint32_t index) {
  return "Pass" + std::to_string(index + 1) + "Enabled";
}

std::string ProfileNameKey(uint32_t index) {
  return "UserPreset." + std::to_string(index) + ".Name";
}

std::string ProfileDataPrefix(uint32_t index) {
  return "UserPreset." + std::to_string(index) + ".Data.";
}

uint32_t ReadProfileCount() {
  int count = 0;
  ReadValue("", "UserPresetCount", count);
  return static_cast<uint32_t>(std::clamp(
      count, 0, static_cast<int>(kMaxSavedProfiles)));
}

void ReadProfileName(uint32_t index, char* name, size_t capacity) {
  if (name == nullptr || capacity == 0) return;
  name[0] = '\0';
  size_t size = capacity;
  const std::string key = ProfileNameKey(index);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(), name,
                            &size);
  name[capacity - 1] = '\0';
}

void WriteProfileName(uint32_t index, const char* name) {
  const std::string key = ProfileNameKey(index);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                            name != nullptr ? name : "");
}

void LoadSettingsValues(Settings& settings, const std::string& key_prefix,
                        bool load_ui_state) {
  ReadValue(key_prefix, "Enabled", settings.enabled);
  ReadValue(key_prefix, "OnlySuperSampling", settings.only_super_sampling);
  ReadValue(key_prefix, "ShowTooltips", settings.show_tooltips);
  ReadValue(key_prefix, "CloneResources", settings.clone_resources);
  ReadValue(key_prefix, "DebugView", settings.debug_view);
  if (settings.debug_view < 0) settings.debug_view = 0;

  LoadHdrSettings(settings.hdr, key_prefix);
  LoadImportanceMaskSettings(settings.importance_mask, key_prefix);
  LoadGrade(key_prefix + "Grade.Backbuffer.", settings.grade_backbuffer);

  if (load_ui_state) {
    ReadValue(key_prefix, "UI.StatusOpen", settings.ui_status_open);
    ReadValue(key_prefix, "UI.PreferencesOpen", settings.ui_preferences_open);
    ReadValue(key_prefix, "UI.PresetsOpen", settings.ui_presets_open);
    ReadValue(key_prefix, "UI.PassesOpen", settings.ui_passes_open);
    ReadValue(key_prefix, "UI.DebugOpen", settings.ui_debug_open);
    ReadValue(key_prefix, "UI.HdrOpen", settings.ui_hdr_open);
    ReadValue(key_prefix, "UI.GradeOpen", settings.ui_grade_open);
    ReadValue(key_prefix, "UI.HdrDiagnosticsOpen", settings.ui_hdr_diagnostics_open);
    ReadValue(key_prefix, "UI.BoundedTuningOpen", settings.ui_bounded_tuning_open);
    ReadValue(key_prefix, "UI.ImportanceMaskOpen", settings.ui_importance_mask_open);
    ReadValue(key_prefix, "UI.ModelOpen", settings.ui_model_open);
    ReadValue(key_prefix, "UI.StrengthOpen", settings.ui_strength_open);
    ReadValue(key_prefix, "UI.SoftCompressionOpen", settings.ui_soft_compression_open);
    ReadValue(key_prefix, "UI.PassAdvancedOpen", settings.ui_pass_advanced_open);
    ReadValue(key_prefix, "UI.ResidualOpen", settings.ui_residual_open);
    ReadValue(key_prefix, "UI.RcasOpen", settings.ui_rcas_open);
    ReadValue(key_prefix, "UI.GradeInputOpen", settings.ui_grade_input_open);
    ReadValue(key_prefix, "UI.GradeOutputOpen", settings.ui_grade_output_open);
  }

  int pass_count = static_cast<int>(settings.pass_count);
  ReadValue(key_prefix, "PassCount", pass_count);
  settings.pass_count = static_cast<uint32_t>(std::clamp(
      pass_count, 1, static_cast<int>(kMaxPasses)));
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (i < settings.pass_count) {
      const std::string key = PassEnabledKey(i);
      ReadValue(key_prefix, key.c_str(), settings.pass_enabled[i]);
      LoadPassSettings(i, settings.passes[i], key_prefix);
    } else {
      settings.pass_enabled[i] = false;
    }
  }

  // Pass zero is the inheritance root; it cannot inherit from another pass.
  settings.passes[0].inherit_working_scale = false;
  settings.passes[0].inherit_intensity = false;
  settings.passes[0].inherit_local_tone = false;
  settings.passes[0].inherit_local_structure = false;
  settings.passes[0].inherit_skin_structure = false;
  settings.passes[0].inherit_luma_strength = false;
  settings.passes[0].inherit_chroma_strength = false;
  settings.passes[0].inherit_soft_max_brighten = false;
  settings.passes[0].inherit_soft_max_darken = false;
  settings.passes[0].inherit_soft_max_chroma = false;
  settings.passes[0].inherit_soft_knee = false;
}

void SaveSettingsValues(const Settings& settings, const std::string& key_prefix,
                        bool save_ui_state) {
  WriteValue(key_prefix, "Enabled", settings.enabled);
  WriteValue(key_prefix, "OnlySuperSampling", settings.only_super_sampling);
  WriteValue(key_prefix, "ShowTooltips", settings.show_tooltips);
  WriteValue(key_prefix, "CloneResources", settings.clone_resources);
  WriteValue(key_prefix, "DebugView", settings.debug_view);

  SaveHdrSettings(settings.hdr, key_prefix);
  SaveImportanceMaskSettings(settings.importance_mask, key_prefix);
  SaveGrade(key_prefix + "Grade.Backbuffer.", settings.grade_backbuffer);

  if (save_ui_state) {
    WriteValue(key_prefix, "UI.StatusOpen", settings.ui_status_open);
    WriteValue(key_prefix, "UI.PreferencesOpen", settings.ui_preferences_open);
    WriteValue(key_prefix, "UI.PresetsOpen", settings.ui_presets_open);
    WriteValue(key_prefix, "UI.PassesOpen", settings.ui_passes_open);
    WriteValue(key_prefix, "UI.DebugOpen", settings.ui_debug_open);
    WriteValue(key_prefix, "UI.HdrOpen", settings.ui_hdr_open);
    WriteValue(key_prefix, "UI.GradeOpen", settings.ui_grade_open);
    WriteValue(key_prefix, "UI.HdrDiagnosticsOpen", settings.ui_hdr_diagnostics_open);
    WriteValue(key_prefix, "UI.BoundedTuningOpen", settings.ui_bounded_tuning_open);
    WriteValue(key_prefix, "UI.ImportanceMaskOpen", settings.ui_importance_mask_open);
    WriteValue(key_prefix, "UI.ModelOpen", settings.ui_model_open);
    WriteValue(key_prefix, "UI.StrengthOpen", settings.ui_strength_open);
    WriteValue(key_prefix, "UI.SoftCompressionOpen", settings.ui_soft_compression_open);
    WriteValue(key_prefix, "UI.PassAdvancedOpen", settings.ui_pass_advanced_open);
    WriteValue(key_prefix, "UI.ResidualOpen", settings.ui_residual_open);
    WriteValue(key_prefix, "UI.RcasOpen", settings.ui_rcas_open);
    WriteValue(key_prefix, "UI.GradeInputOpen", settings.ui_grade_input_open);
    WriteValue(key_prefix, "UI.GradeOutputOpen", settings.ui_grade_output_open);
  }

  WriteValue(key_prefix, "PassCount", static_cast<int>(settings.pass_count));
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    const std::string key = PassEnabledKey(i);
    WriteValue(key_prefix, key.c_str(), settings.pass_enabled[i]);
    SavePassSettings(i, settings.passes[i], key_prefix);
  }
}

bool ValidProfileIndex(uint32_t index) {
  return index < ReadProfileCount();
}

}  // namespace

Settings& GetSettings() {
  return g_settings;
}

const PassSettings& GetResolvedPass(uint32_t index) {
  if (index >= kMaxPasses) {
    static const PassSettings kEmpty;
    return kEmpty;
  }
  return g_resolved[index];
}

void LoadSettings() {
  LoadSettingsValues(g_settings, "", true);
  DlssGuides::Get().RequestCloneResources(g_settings.clone_resources);
  RecomputeResolved();
  TraceSettings(g_settings, g_resolved);
}

void SaveSettings() {
  RecomputeResolved();
  TraceSettings(g_settings, g_resolved);
  SaveSettingsValues(g_settings, "", true);
}

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity) {
  const uint32_t count = ReadProfileCount();
  if (profiles != nullptr) {
    const uint32_t available = std::min(count, capacity);
    for (uint32_t i = 0; i < available; ++i) {
      ReadProfileName(i, profiles[i].name, sizeof(profiles[i].name));
    }
  }
  return count;
}

bool SaveCurrentAsProfile(const char* name) {
  if (name == nullptr) return false;
  std::string clean_name(name);
  const auto first = clean_name.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  const auto last = clean_name.find_last_not_of(" \t\r\n");
  clean_name = clean_name.substr(first, last - first + 1);
  if (clean_name.size() >= kSavedProfileNameCapacity) return false;

  const uint32_t count = ReadProfileCount();
  if (count >= kMaxSavedProfiles) return false;
  for (uint32_t i = 0; i < count; ++i) {
    char existing[kSavedProfileNameCapacity] = {};
    ReadProfileName(i, existing, sizeof(existing));
    if (clean_name == existing) return false;
  }

  WriteProfileName(count, clean_name.c_str());
  SaveSettingsValues(g_settings, ProfileDataPrefix(count), false);
  WriteValue("", "UserPresetCount", static_cast<int>(count + 1));
  return true;
}

bool SaveCurrentToProfile(uint32_t index) {
  if (!ValidProfileIndex(index)) return false;
  SaveSettingsValues(g_settings, ProfileDataPrefix(index), false);
  return true;
}

bool LoadSavedProfile(uint32_t index) {
  if (!ValidProfileIndex(index)) return false;
  Settings loaded = g_settings;
  LoadSettingsValues(loaded, ProfileDataPrefix(index), false);
  g_settings = loaded;
  if (g_settings.active_pass < 0) g_settings.active_pass = 0;
  if (g_settings.active_pass >= static_cast<int>(g_settings.pass_count)) {
    g_settings.active_pass = static_cast<int>(g_settings.pass_count) - 1;
  }
  DlssGuides::Get().RequestCloneResources(g_settings.clone_resources);
  SaveSettings();
  return true;
}

bool DeleteSavedProfile(uint32_t index) {
  const uint32_t count = ReadProfileCount();
  if (index >= count) return false;

  for (uint32_t i = index; i + 1 < count; ++i) {
    char next_name[kSavedProfileNameCapacity] = {};
    ReadProfileName(i + 1, next_name, sizeof(next_name));
    Settings data = g_settings;
    LoadSettingsValues(data, ProfileDataPrefix(i + 1), false);
    SaveSettingsValues(data, ProfileDataPrefix(i), false);
    WriteProfileName(i, next_name);
  }

  WriteProfileName(count - 1, "");
  WriteValue("", "UserPresetCount", static_cast<int>(count - 1));
  return true;
}

}  // namespace reshade_dlssnr

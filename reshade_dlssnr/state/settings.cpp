#include "state/settings.h"
#include "hooks/dlss_guides.h"

#include <reshade.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

#include "compose/settings.h"
#include "ctl/usage/config.h"
#include "ctl/usage/ui_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/hdr/settings.h"
#include "feature/importance_mask/settings.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/preferences/settings.h"
#include "grade/settings.h"
#include "state/trace.h"

namespace reshade_dlssnr {

namespace {
Settings g_settings;

template <typename T>
void ReadValue(const std::string& root, const char* key, T& value) {
  const std::string full_key = root + CStrToString(key);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}

// A string needs the char* overload: ReShade's scalar template goes through
// std::from_chars, which has no std::string form (it fails to compile), and a
// name is worth more than a 31-character buffer anyway.
inline void ReadValue(const std::string& root, const char* key,
                      std::string& value) {
  const std::string full_key = root + CStrToString(key);
  char buffer[256] = "";
  size_t size = sizeof(buffer);
  if (reshade::get_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(),
                                buffer, &size)) {
    value.assign(buffer);
  }
}


template <typename T>
void WriteValue(const std::string& root, const char* key, const T& value) {
  const std::string full_key = root + CStrToString(key);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(), value);
}

// A string needs the char* overload: ReShade's scalar template goes through
// std::to_chars, which has no std::string form (it fails to compile).
inline void WriteValue(const std::string& root, const char* key,
                       const std::string& value) {
  const std::string full_key = root + CStrToString(key);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", full_key.c_str(),
                            value.c_str());
}

constexpr ProfileId kFirstProfileId = 1;
constexpr const char* kProfileOrderKey = "UserPresetOrder";
constexpr const char* kNextProfileIdKey = "UserPresetNextId";
constexpr const char* kNextPassIdKey = "PassNextId";

std::string ProfilePrefix(ProfileId id) {
  return "UserPreset." + std::to_string(id);
}

std::string ProfileValuePrefix(ProfileId id) {
  return ProfilePrefix(id) + ".";
}

std::string ProfileDataPrefix(ProfileId id) {
  return ProfilePrefix(id) + ".Data";
}

std::string ProfileNameKey(ProfileId id) {
  return ProfileValuePrefix(id) + "Name";
}

std::vector<ProfileId> ParseProfileOrder(const std::string& text) {
  std::vector<ProfileId> order;
  size_t begin = 0;
  while (begin <= text.size() && order.size() < kMaxSavedProfiles) {
    const size_t comma = text.find(',', begin);
    const std::string token = text.substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!token.empty()) {
      char* end = nullptr;
      const unsigned long long parsed = std::strtoull(token.c_str(), &end, 10);
      if (end != token.c_str() && *end == '\0' &&
          parsed >= kFirstProfileId &&
          parsed <= std::numeric_limits<ProfileId>::max()) {
        const ProfileId id = static_cast<ProfileId>(parsed);
        if (std::find(order.begin(), order.end(), id) == order.end()) {
          order.push_back(id);
        }
      }
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return order;
}

std::vector<ProfileId> ReadProfileOrder() {
  std::string text;
  ReadValue("", kProfileOrderKey, text);
  return ParseProfileOrder(text);
}

void WriteProfileOrder(const std::vector<ProfileId>& order) {
  std::string text;
  for (ProfileId id : order) {
    if (!text.empty()) text += ',';
    text += std::to_string(id);
  }
  WriteValue("", kProfileOrderKey, text);
}

void ReadProfileName(ProfileId id, char* name, size_t capacity) {
  if (name == nullptr || capacity == 0) return;
  name[0] = '\0';
  size_t size = capacity;
  const std::string key = ProfileNameKey(id);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(), name,
                            &size);
  name[capacity - 1] = '\0';
}

void WriteProfileName(ProfileId id, const char* name) {
  const std::string key = ProfileNameKey(id);
  const std::string safe_name = CStrToString(name);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", key.c_str(),
                            safe_name.c_str());
}

bool ValidProfileId(ProfileId id) {
  const std::vector<ProfileId> order = ReadProfileOrder();
  return std::find(order.begin(), order.end(), id) != order.end();
}

// The pass order is one config value: a comma-separated list of stable pass
// ids. Position is not recorded, because the vector IS the order.
constexpr const char* kPassOrderKey = "PassOrder";

std::string PassOrderText(const std::vector<feature::pass::PassId>& order) {
  std::string text;
  for (feature::pass::PassId id : order) {
    if (!text.empty()) text += ',';
    text += std::to_string(id);
  }
  return text;
}

std::vector<feature::pass::PassId> ParsePassOrder(const std::string& text,
                                                  bool ensure_nonempty = true) {
  std::vector<feature::pass::PassId> order;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t comma = text.find(',', begin);
    const std::string token =
        text.substr(begin, comma == std::string::npos ? std::string::npos
                                                      : comma - begin);
    if (!token.empty()) {
      const long long id = std::strtoll(token.c_str(), nullptr, 10);
      if (id > 0) {
        const feature::pass::PassId pid =
            static_cast<feature::pass::PassId>(id);
        if (std::find(order.begin(), order.end(), pid) == order.end()) {
          order.push_back(pid);
        }
      }
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  if (order.size() > kMaxPasses || (ensure_nonempty && order.empty())) {
    order.clear();
    if (ensure_nonempty) order.push_back(NewPassId());
  }
  return order;
}

std::vector<feature::pass::PassId> ReadProfilePassOrder(ProfileId id) {
  std::string text;
  ReadValue(ProfileValuePrefix(id), kPassOrderKey, text);
  return ParsePassOrder(text, false);
}

void LoadSettingsValues(Settings& settings, const std::string& key_prefix,
                        bool load_ui_state) {
  ReadValue(key_prefix, "Enabled", settings.enabled);

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
    ReadValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    ReadValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    ReadValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    ReadValue(key_prefix, "UI.SoftCompressionOpen", settings.ui_soft_compression_open);
    ReadValue(key_prefix, "UI.PassAdvancedOpen", settings.ui_pass_advanced_open);
    ReadValue(key_prefix, "UI.ResidualOpen", settings.ui_residual_open);
    ReadValue(key_prefix, "UI.RcasOpen", settings.ui_rcas_open);
    ReadValue(key_prefix, "UI.GradeInputOpen", settings.ui_grade_input_open);
    ReadValue(key_prefix, "UI.GradeOutputOpen", settings.ui_grade_output_open);
  }

  // Pass order first: the store's key source needs it, because it is what
  // decides which instances exist.
  std::string order_text;
  ReadValue(key_prefix, kPassOrderKey, order_text);
  settings.pass_order = ParsePassOrder(order_text);
  // active_pass_id is not persisted: fall back to the first pass when
  // the previously-selected id is not in the freshly loaded list.
  if (std::find(settings.pass_order.begin(), settings.pass_order.end(),
                settings.active_pass_id) == settings.pass_order.end()) {
    settings.active_pass_id =
        settings.pass_order.empty() ? 0 : settings.pass_order.front();
  }
}

void SaveSettingsValues(const Settings& settings, const std::string& key_prefix,
                        bool save_ui_state) {
  WriteValue(key_prefix, "Enabled", settings.enabled);
  WriteValue(key_prefix, kPassOrderKey,
             PassOrderText(settings.pass_order));

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
    WriteValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    WriteValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    WriteValue(key_prefix, "UI.PassTemporalOpen", settings.ui_pass_temporal_open);
    WriteValue(key_prefix, "UI.SoftCompressionOpen", settings.ui_soft_compression_open);
    WriteValue(key_prefix, "UI.PassAdvancedOpen", settings.ui_pass_advanced_open);
    WriteValue(key_prefix, "UI.ResidualOpen", settings.ui_residual_open);
    WriteValue(key_prefix, "UI.RcasOpen", settings.ui_rcas_open);
    WriteValue(key_prefix, "UI.GradeInputOpen", settings.ui_grade_input_open);
    WriteValue(key_prefix, "UI.GradeOutputOpen", settings.ui_grade_output_open);
  }
}

// Instance keys for the instanced sections, supplied to ctl::Load.
//
// ctl does not track instance count -- see the non-goal in ctl/serialize.h.
// How many passes exist is application state, and it is already here in
// Settings, so it is this function's job to hand the keys over rather than
// have the loader guess them from a config file it cannot enumerate.
std::vector<ctl::InstanceKey> InstanceKeysForSections(
    const void* context, const std::string& section) {
  const Settings& settings = *static_cast<const Settings*>(context);
  // Pass and its two grade sections are all indexed by pass.
  const bool pass_indexed = section == feature::pass::kSection ||
                            section == feature::pass::kGradeInputSection ||
                            section == feature::pass::kGradeOutputSection;
  if (!pass_indexed) return {};

  // The order IS the key list: one instance per pass in pass_order, each under
  // its own stable id. Position never enters a key, which is what makes a
  // reorder free.
  std::vector<ctl::InstanceKey> keys;
  keys.reserve(settings.pass_order.size());
  for (feature::pass::PassId id : settings.pass_order) {
    keys.push_back(feature::pass::PassKey(id));
  }
  return keys;
}



// Load the store from `prefix`, replacing whatever it holds. Replacing
// matters: a profile load must produce exactly the profile's state, not the
// current state merged with the profile's overrides.
void LoadStore(const Settings& settings, const std::string& prefix) {
  ctl::ValueStore& store = ctl::Store();
  store.ResetToDefaults();
  ctl::ReshadeConfigReader reader;
  ctl::Load(store, reader, prefix, &InstanceKeysForSections, &settings);
}

void SaveStore(const Settings& settings, const std::string& prefix) {
  ctl::ValueStore& store = ctl::Store();
  ctl::ReshadeConfigWriter writer;
  // The Pass section is scoped to pass_order on purpose, excluding stale live instances.
  ctl::Save(store, writer, prefix, &InstanceKeysForSections, &settings);
}

}  // namespace

Settings& GetSettings() {
  return g_settings;
}

feature::pass::PassId NewPassId() {
  uint64_t next = 1;
  std::string next_text;
  ReadValue("", kNextPassIdKey, next_text);
  if (!next_text.empty()) {
    char* end = nullptr;
    const unsigned long long parsed =
        std::strtoull(next_text.c_str(), &end, 10);
    if (end != next_text.c_str() && *end == '\0' && parsed >= 1) {
      next = parsed;
    }
  }
  // The counter is the source of truth, but the current pass list is
  // also consulted so a hand-edited or stale counter cannot hand out
  // an id that is already in play.
  for (feature::pass::PassId id : g_settings.pass_order) {
    if (static_cast<uint64_t>(id) >= next) {
      next = static_cast<uint64_t>(id) + 1;
    }
  }
  const feature::pass::PassId id =
      static_cast<feature::pass::PassId>(next);
  WriteValue("", kNextPassIdKey, std::to_string(next + 1));
  return id;
}

ProfileId NewProfileId() {
  uint64_t next = kFirstProfileId;
  std::string next_text;
  ReadValue("", kNextProfileIdKey, next_text);
  if (!next_text.empty()) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(next_text.c_str(), &end, 10);
    if (end != next_text.c_str() && *end == '\0' && parsed >= kFirstProfileId) {
      next = parsed;
    }
  }

  // The persisted high-water mark includes deleted profiles. The order list
  // also protects against a stale/missing counter when profiles still exist.
  for (ProfileId id : ReadProfileOrder()) {
    if (static_cast<uint64_t>(id) >= next) {
      next = static_cast<uint64_t>(id) + 1;
    }
  }
  if (next > std::numeric_limits<ProfileId>::max()) return 0;

  const ProfileId id = static_cast<ProfileId>(next);
  WriteValue("", kNextProfileIdKey, std::to_string(next + 1));
  return id;
}

void LoadSettings() {
  // Read the stable pass order first, because it determines which instance
  // keys the store loads.
  LoadSettingsValues(g_settings, "", true);
  LoadStore(g_settings, "");
  {
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    const bool clone_resources = ctl::Store().Read(
        feature::preferences::kSection, shared,
        feature::preferences::kCloneResources).as_bool;
    DlssGuides::Get().RequestCloneResources(clone_resources);
  }
  TraceSettings(g_settings);
}

void SaveSettings() {
  TraceSettings(g_settings);
  SaveSettingsValues(g_settings, "", true);
  SaveStore(g_settings, "");
}

void ErasePassKeys(feature::pass::PassId id) {
  // The Pass section and its two hosted grade sections are all keyed by pass.
  // Their persisted layout is documented in ctl/serialize.h:
  //   <prefix>.S.<Section>.K.<keytext>.<setting>
  // With an empty prefix this becomes "S.<Section>.K.<keytext>.<setting>".
  // Only settings marked persist are written by Save, so only those need
  // erasing here.
  ctl::ValueStore& store = ctl::Store();
  const ctl::Schema& schema = store.schema();
  ctl::ReshadeConfigWriter writer;
  const ctl::InstanceKey key = feature::pass::PassKey(id);
  const std::string key_text = key.ToText();

  const char* pass_sections[] = {
      feature::pass::kSection,
      feature::pass::kGradeInputSection,
      feature::pass::kGradeOutputSection,
  };
  for (const char* section_name : pass_sections) {
    const ctl::Section* section = schema.FindSection(section_name);
    if (section == nullptr) continue;
    const std::string base =
        std::string("S.") + section_name + ".K." + key_text;
    for (const auto& setting : section->settings()) {
      if (!setting->meta().persist) continue;
      writer.Erase(base + "." + setting->meta().name);
    }
  }
}

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity) {
  const std::vector<ProfileId> order = ReadProfileOrder();
  const uint32_t count = static_cast<uint32_t>(order.size());
  if (profiles != nullptr) {
    const uint32_t available = std::min(count, capacity);
    for (uint32_t i = 0; i < available; ++i) {
      profiles[i].id = order[i];
      ReadProfileName(profiles[i].id, profiles[i].name,
                      sizeof(profiles[i].name));
    }
  }
  return count;
}

bool SaveCurrentAsProfile(const char* name) {
  std::string clean_name = CStrToString(name);
  const auto first = clean_name.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  const auto last = clean_name.find_last_not_of(" \t\r\n");
  clean_name = clean_name.substr(first, last - first + 1);
  if (clean_name.size() >= kSavedProfileNameCapacity) return false;

  std::vector<ProfileId> order = ReadProfileOrder();
  if (order.size() >= kMaxSavedProfiles) return false;
  for (ProfileId id : order) {
    char existing[kSavedProfileNameCapacity] = {};
    ReadProfileName(id, existing, sizeof(existing));
    if (clean_name == existing) return false;
  }

  const ProfileId id = NewProfileId();
  if (id == 0) return false;
  const std::string value_prefix = ProfileValuePrefix(id);
  WriteProfileName(id, clean_name.c_str());
  SaveSettingsValues(g_settings, value_prefix, false);
  SaveStore(g_settings, ProfileDataPrefix(id));
  order.push_back(id);
  WriteProfileOrder(order);
  return true;
}

bool RenameSavedProfile(ProfileId id, const char* name) {
  if (!ValidProfileId(id)) return false;

  std::string clean_name = CStrToString(name);
  const auto first = clean_name.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  const auto last = clean_name.find_last_not_of(" \t\r\n");
  clean_name = clean_name.substr(first, last - first + 1);
  if (clean_name.size() >= kSavedProfileNameCapacity) return false;

  for (ProfileId other_id : ReadProfileOrder()) {
    if (other_id == id) continue;
    char existing[kSavedProfileNameCapacity] = {};
    ReadProfileName(other_id, existing, sizeof(existing));
    if (clean_name == existing) return false;
  }

  WriteProfileName(id, clean_name.c_str());
  return true;
}

bool SaveCurrentToProfile(ProfileId id) {
  if (!ValidProfileId(id)) return false;

  // Remove all schema-defined keys from the old pass set before overwriting
  // the profile. This prevents removed pass ids from leaving stale overrides
  // behind when the profile is saved with fewer or different passes.
  Settings previous;
  previous.pass_order = ReadProfilePassOrder(id);
  ctl::ReshadeConfigWriter writer;
  ctl::Delete(ctl::Store(), writer, ProfileDataPrefix(id),
              &InstanceKeysForSections, &previous);

  SaveSettingsValues(g_settings, ProfileValuePrefix(id), false);
  SaveStore(g_settings, ProfileDataPrefix(id));
  return true;
}

bool LoadSavedProfile(ProfileId id) {
  if (!ValidProfileId(id)) return false;
  Settings loaded = g_settings;
  LoadSettingsValues(loaded, ProfileValuePrefix(id), false);
  // The store is process-wide, so loading a profile has to replace it, not
  // merge into it -- otherwise the previous profile's HDR/mask overrides
  // would survive into the one being loaded.
  // Profile blocks use the stable-id schema only. Do not import old positional
  // profile fields into a newly created block that happens to reuse its number.
  LoadStore(loaded, ProfileDataPrefix(id));
  g_settings = loaded;
  if (std::find(g_settings.pass_order.begin(), g_settings.pass_order.end(),
                g_settings.active_pass_id) == g_settings.pass_order.end()) {
    g_settings.active_pass_id =
        g_settings.pass_order.empty() ? 0 : g_settings.pass_order.front();
  }
  {
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    const bool clone_resources = ctl::Store().Read(
        feature::preferences::kSection, shared,
        feature::preferences::kCloneResources).as_bool;
    DlssGuides::Get().RequestCloneResources(clone_resources);
  }
  SaveSettings();
  return true;
}

bool DeleteSavedProfile(ProfileId id) {
  std::vector<ProfileId> order = ReadProfileOrder();
  const auto found = std::find(order.begin(), order.end(), id);
  if (found == order.end()) return false;

  order.erase(found);
  WriteProfileOrder(order);

  // The stored pass order is the instance-key source for this profile's
  // instanced ctl sections. Deletion does not modify the active store.
  Settings profile;
  profile.pass_order = ReadProfilePassOrder(id);
  ctl::ReshadeConfigWriter writer;
  ctl::Delete(ctl::Store(), writer, ProfileDataPrefix(id),
              &InstanceKeysForSections, &profile);

  const std::string value_prefix = ProfileValuePrefix(id);
  writer.Erase(ProfileNameKey(id));
  writer.Erase(value_prefix + "Enabled");
  writer.Erase(value_prefix + kPassOrderKey);

  return true;
}

}  // namespace reshade_dlssnr

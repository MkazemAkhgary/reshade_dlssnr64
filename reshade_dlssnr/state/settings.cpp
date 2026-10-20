#include "state/settings.h"
#include "hooks/dlss_guides.h"

#include <reshade.hpp>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <string>

#include "compose/settings.h"
#include "ctl/usage/config.h"
#include "feature/settings_store.h"
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

void LoadSettingsValues(Settings& settings, const std::string& key_prefix) {
  ReadValue(key_prefix, "Enabled", settings.enabled);
}

void SaveSettingsValues(const Settings& settings,
                        const std::string& key_prefix) {
  WriteValue(key_prefix, "Enabled", settings.enabled);
}

feature::pass::PassOrder ReadPassOrder(const ctl::ValueStore& store) {
  const ctl::Value value = store.Read(
      feature::pass::kOrderSection, ctl::InstanceKey::FromInt(0),
      feature::pass::kOrderSetting);
  feature::pass::PassOrder order;
  order.reserve(value.as_int_list.size());
  for (int64_t id : value.as_int_list) {
    if (id <= 0 || id > std::numeric_limits<feature::pass::PassId>::max()) {
      continue;
    }
    const auto pass_id = static_cast<feature::pass::PassId>(id);
    if (std::find(order.begin(), order.end(), pass_id) == order.end()) {
      order.push_back(pass_id);
    }
    if (order.size() >= kMaxPasses) break;
  }
  return order;
}

void WritePassOrder(ctl::ValueStore& store,
                    const feature::pass::PassOrder& order) {
  std::vector<int64_t> values;
  values.reserve(order.size());
  for (feature::pass::PassId id : order) values.push_back(id);
  store.Write(feature::pass::kOrderSection, ctl::InstanceKey::FromInt(0),
              feature::pass::kOrderSetting,
              ctl::Value::IntList(std::move(values)));
}

void SyncPassOrder(Settings& settings, bool ensure_nonempty) {
  feature::pass::PassOrder order = ReadPassOrder(feature::SettingsStore());
  if (order.empty() && ensure_nonempty) {
    order.push_back(NewPassId());
    WritePassOrder(feature::SettingsStore(), order);
  }
  settings.pass_order = std::move(order);
  if (std::find(settings.pass_order.begin(), settings.pass_order.end(),
                settings.active_pass_id) == settings.pass_order.end()) {
    settings.active_pass_id =
        settings.pass_order.empty() ? 0 : settings.pass_order.front();
  }
}

// The key source reads the pass-order setting directly from the store. The
// Pass Order section is registered before every pass-keyed instanced section
// (Pass, its two model grades, the four HDR sections, and Importance Mask)
// so ctl's schema-order load has already populated it when those sections
// ask for their instance keys.
std::vector<ctl::InstanceKey> InstanceKeysForSections(
    const void* context, const std::string& section) {
  const bool pass_indexed = section == feature::pass::kSection ||
                            section == feature::pass::kGradeInputSection ||
                            section == feature::pass::kGradeOutputSection ||
                            section == feature::hdr::kSection ||
                            section == feature::hdr::kGameSection ||
                            section == feature::hdr::kAdaptiveSection ||
                            section == feature::hdr::kLocalSection ||
                            section == feature::importance_mask::kSection;
  if (!pass_indexed) return {};

  const auto& store = *static_cast<const ctl::ValueStore*>(context);
  const feature::pass::PassOrder order = ReadPassOrder(store);
  std::vector<ctl::InstanceKey> keys;
  keys.reserve(order.size());
  for (feature::pass::PassId id : order) {
    keys.push_back(feature::pass::PassKey(id));
  }
  return keys;
}

// A fresh store is used when inspecting a profile's instance list. Loading
// only its single-instance settings is enough to resolve Pass Order without
// instantiating or merging the live pass sections.
void LoadProfileOrderStore(ctl::ValueStore& store, ProfileId id) {
  ctl::ReshadeConfigReader reader;
  ctl::Load(store, reader, ProfileDataPrefix(id), nullptr, nullptr,
            ctl::PersistenceTarget::Profile);
}

void LoadStore(Settings& settings, const std::string& prefix,
               ctl::PersistenceTarget target) {
  ctl::ValueStore& store = feature::SettingsStore();
  store.ResetToDefaults(target);
  ctl::ReshadeConfigReader reader;
  ctl::Load(store, reader, prefix, &InstanceKeysForSections, &store, target);
  SyncPassOrder(settings, true);
}

void SaveStore(const Settings& settings, const std::string& prefix,
               ctl::PersistenceTarget target) {
  ctl::ValueStore& store = feature::SettingsStore();
  WritePassOrder(store, settings.pass_order);
  ctl::ReshadeConfigWriter writer;
  ctl::Save(store, writer, prefix, &InstanceKeysForSections, &store, target);
}

}  // namespace

Settings& GetSettings() {
  return g_settings;
}

void SetPassOrder(Settings& settings, feature::pass::PassOrder order) {
  if (order.size() > kMaxPasses) order.resize(kMaxPasses);
  settings.pass_order = std::move(order);
  WritePassOrder(feature::SettingsStore(), settings.pass_order);
  if (std::find(settings.pass_order.begin(), settings.pass_order.end(),
                settings.active_pass_id) == settings.pass_order.end()) {
    settings.active_pass_id =
        settings.pass_order.empty() ? 0 : settings.pass_order.front();
  }
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
  // ctl loads the order setting before the instanced sections, then uses it
  // to resolve the active pass keys for this profile.
  LoadSettingsValues(g_settings, "");
  LoadStore(g_settings, "", ctl::PersistenceTarget::Global);
  {
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    const bool clone_resources = feature::SettingsStore().Read(
        feature::preferences::kSection, shared,
        feature::preferences::kCloneResources).as_bool;
    DlssGuides::Get().RequestCloneResources(clone_resources);
  }
  TraceSettings(g_settings);
}

void SaveSettings() {
  TraceSettings(g_settings);
  SaveSettingsValues(g_settings, "");
  SaveStore(g_settings, "", ctl::PersistenceTarget::Global);
}

void ErasePassKeys(feature::pass::PassId id) {
  // Every pass-keyed section erases here: the Pass section itself, its two
  // hosted model grades, the four HDR sections, and Importance Mask. Their
  // persisted layout is documented in ctl/serialize.h:
  //   <prefix>.S.<Section>.K.<keytext>.<setting>
  // With an empty prefix this becomes "S.<Section>.K.<keytext>.<setting>".
  // Only settings marked persist are written by Save, so only those need
  // erasing here.
  ctl::ValueStore& store = feature::SettingsStore();
  const ctl::Schema& schema = store.schema();
  ctl::ReshadeConfigWriter writer;
  const ctl::InstanceKey key = feature::pass::PassKey(id);
  const std::string key_text = key.ToText();

  const char* pass_sections[] = {
      feature::pass::kSection,
      feature::pass::kGradeInputSection,
      feature::pass::kGradeOutputSection,
      feature::hdr::kSection,
      feature::hdr::kGameSection,
      feature::hdr::kAdaptiveSection,
      feature::hdr::kLocalSection,
      feature::importance_mask::kSection,
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
    store.EraseInstance(section_name, key);
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
  SaveSettingsValues(g_settings, value_prefix);
  SaveStore(g_settings, ProfileDataPrefix(id), ctl::PersistenceTarget::Profile);
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
  ctl::ValueStore previous(feature::SettingsStore().schema());
  LoadProfileOrderStore(previous, id);
  ctl::ReshadeConfigWriter writer;
  ctl::Delete(feature::SettingsStore(), writer, ProfileDataPrefix(id),
              &InstanceKeysForSections, &previous,
              ctl::PersistenceTarget::Profile);

  SaveSettingsValues(g_settings, ProfileValuePrefix(id));
  SaveStore(g_settings, ProfileDataPrefix(id), ctl::PersistenceTarget::Profile);
  return true;
}

bool LoadSavedProfile(ProfileId id) {
  if (!ValidProfileId(id)) return false;
  Settings loaded = g_settings;
  LoadSettingsValues(loaded, ProfileValuePrefix(id));
  // The store is process-wide, so loading a profile has to replace it, not
  // merge into it -- otherwise the previous profile's HDR/mask overrides
  // would survive into the one being loaded.
  // Profile blocks use the stable-id schema only. Do not import old positional
  // profile fields into a newly created block that happens to reuse its number.
  LoadStore(loaded, ProfileDataPrefix(id), ctl::PersistenceTarget::Profile);
  g_settings = loaded;
  if (std::find(g_settings.pass_order.begin(), g_settings.pass_order.end(),
                g_settings.active_pass_id) == g_settings.pass_order.end()) {
    g_settings.active_pass_id =
        g_settings.pass_order.empty() ? 0 : g_settings.pass_order.front();
  }
  {
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    const bool clone_resources = feature::SettingsStore().Read(
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

  // Resolve the profile's own instance set, then let ctl erase every
  // registered profile-persisted setting for exactly those instances.
  ctl::ValueStore profile(feature::SettingsStore().schema());
  LoadProfileOrderStore(profile, id);
  ctl::ReshadeConfigWriter writer;
  ctl::Delete(feature::SettingsStore(), writer, ProfileDataPrefix(id),
              &InstanceKeysForSections, &profile,
              ctl::PersistenceTarget::Profile);

  const std::string value_prefix = ProfileValuePrefix(id);
  writer.Erase(ProfileNameKey(id));
  writer.Erase(value_prefix + "Enabled");

  return true;
}

}  // namespace reshade_dlssnr

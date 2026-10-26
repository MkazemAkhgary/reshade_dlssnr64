#include "state/settings.h"
#include "profiling/profile.h"
#include "hooks/dlss_guides.h"

#include <reshade.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "compose/settings.h"
#include "ctl/usage/config.h"
#include "ctl/usage/json.h"
#include "ctl/usage/json_config.h"
#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/hdr/settings.h"
#include "feature/exposure_mask/settings.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/preferences/settings.h"
#include "grade/settings.h"
#include "state/trace.h"

namespace reshade_dlssnr {

namespace {
Settings g_settings;

// ---------------------------------------------------------------------------
// JSON settings store.
//
// The JSON file lives beside Reshade.ini and holds the entire persisted
// settings tree. Pass ids and profile positions are compacted on every save,
// so the on-disk file has no index gaps and no leftover keys from removed
// settings. The INI is consulted only for the store's filename
// (JsonStoreFile).
// ---------------------------------------------------------------------------

constexpr const char* kIniSection = "RESHADE_DLSSNR";
constexpr const char* kJsonFileKey = "JsonStoreFile";
constexpr const char* kDefaultJsonFile = "reshade_dlssnr64.json";
constexpr ProfileId kFirstProfileId = 1;

bool IsValidStoreFileName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  for (char c : name) {
    switch (c) {
      case '\\': case '/': case ':': case '*': case '?':
      case '"': case '<': case '>': case '|':
        return false;
      default:
        break;
    }
  }
  return true;
}

std::filesystem::path ResolveJsonPath() {
  std::string name = kDefaultJsonFile;
  char buffer[260] = {};
  size_t size = sizeof(buffer);
  if (reshade::get_config_value(nullptr, kIniSection, kJsonFileKey,
                                buffer, &size)) {
    const std::string candidate(buffer);
    if (IsValidStoreFileName(candidate)) name = candidate;
  }

  wchar_t exe_path[MAX_PATH] = {};
  const DWORD len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    std::filesystem::path dir(exe_path);
    dir = dir.parent_path();
    if (!dir.empty()) return dir / name;
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  if (!ec) return cwd / name;
  wchar_t temp[MAX_PATH] = {};
  const DWORD temp_len = GetTempPathW(MAX_PATH, temp);
  if (temp_len > 0 && temp_len < MAX_PATH) {
    return std::filesystem::path(temp) / name;
  }
  return name;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out.good()) return false;
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(temp, path, ec);
  }
  return !ec;
}

// Every pass-keyed section. Enumerated here so migration and compaction
// remain in sync when a new pass-keyed section is added.
const char* const kPassSections[] = {
    feature::pass::kSection,
    feature::pass::kGradeInputSection,
    feature::pass::kGradeOutputSection,
    feature::hdr::kSection,
    feature::hdr::kGameSection,
    feature::hdr::kAdaptiveSection,
    feature::hdr::kLocalSection,
    feature::exposure_mask::kSection,
};

// Parse a comma-separated list of integers. Tolerates a JSON number that the
// INI-to-JSON migration produced for Pass Order.ids (as_string() normalizes it
// back to text).
std::vector<int> ParseIntList(const std::string& text) {
  std::vector<int> out;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t comma = text.find(',', begin);
    const std::string part = text.substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!part.empty()) {
      try { out.push_back(std::stoi(part)); } catch (...) {}
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return out;
}

std::vector<int> ReadPassIdsFromTree(json::Value& s_subtree) {
  json::Value* order = s_subtree.find("Pass Order.ids");
  if (order == nullptr) return {};
  return ParseIntList(order->as_string());
}

// Derive pass ids from the Pass section's instance keys. Used when
// Pass Order.ids is absent so compaction cannot silently drop keys.
std::vector<int> DeriveIdsFromK(json::Value& s_subtree) {
  std::vector<int> ids;
  json::Value* k = s_subtree.find("Pass");
  if (k == nullptr || !k->is_object()) return ids;
  for (const auto& entry : k->object()) {
    if (entry.first.empty() || entry.first[0] != 'i') continue;
    try { ids.push_back(std::stoi(entry.first.substr(1))); } catch (...) {}
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

void WritePassIdsToTree(json::Value& s_subtree, const std::vector<int>& ids) {
  std::string text;
  for (int id : ids) {
    if (!text.empty()) text += ',';
    text += std::to_string(id);
  }
  s_subtree.ensure("Pass Order")["ids"] = json::Value(text);
}

// Renumber every pass-keyed section under `s_subtree` (which is an "S"
// subtree) to i1..iN, preserving each instance's values. Ids are derived from
// Pass Order.ids, falling back to the Pass section's own keys so a hand-edited
// file that omitted the order setting cannot lose data.
void CompactPassKeysInTree(json::Value& s_subtree) {
  std::vector<int> from_ids = ReadPassIdsFromTree(s_subtree);
  if (from_ids.empty()) from_ids = DeriveIdsFromK(s_subtree);

  std::unordered_map<std::string, std::string> remap;
  for (size_t i = 0; i < from_ids.size(); ++i) {
    remap["i" + std::to_string(from_ids[i])] = "i" + std::to_string(i + 1);
  }

  for (const char* section_name : kPassSections) {
    json::Value* k = s_subtree.find(section_name);
    if (k == nullptr || !k->is_object()) continue;
    json::Value new_k = json::Value::MakeObject();
    for (const auto& entry : k->object()) {
      auto it = remap.find(entry.first);
      if (it != remap.end()) new_k[it->second] = entry.second;
    }
    *k = std::move(new_k);
  }

  std::vector<int> new_ids;
  for (size_t i = 0; i < from_ids.size(); ++i) {
    new_ids.push_back(static_cast<int>(i + 1));
  }
  WritePassIdsToTree(s_subtree, new_ids);
}

class JsonStore {
 public:
  static JsonStore& Get() {
    static JsonStore instance;
    return instance;
  }

  bool EnsureLoaded() {
    if (loaded_) return true;
    loaded_ = true;
    path_ = ResolveJsonPath();

    std::error_code ec;
    if (std::filesystem::exists(path_, ec)) {
      const std::string text = ReadTextFile(path_);
      if (text.empty()) {
        root_ = json::Value::MakeObject();
        root_["Version"] = json::Value(kSettingsVersion);
        Save();
        return true;
      }
      std::string error;
      root_ = json::Value::parse(text, &error);
      if (error.empty() && !root_.is_object()) {
        error = "root is not an object";
      }
      if (!error.empty()) {
        reshade::log::message(
            reshade::log::level::error,
            ("DLSSNR: JSON settings parse failed: " + error).c_str());
        std::filesystem::path backup = path_;
        backup += ".broken";
        std::filesystem::rename(path_, backup, ec);
        root_ = json::Value::MakeObject();
        root_["Version"] = json::Value(kSettingsVersion);
        Save();
        return true;
      }
    } else {
      root_ = json::Value::MakeObject();
      root_["Version"] = json::Value(kSettingsVersion);
      Save();
      return true;
    }

    return true;
  }

  bool Save() {
    if (!loaded_ || path_.empty()) return false;
    return WriteTextFile(path_, root_.dump());
  }

  json::Value& root() { return root_; }
  const json::Value& root() const { return root_; }

 private:
  JsonStore() = default;

  json::Value root_;
  std::filesystem::path path_;
  bool loaded_ = false;
};

template <typename T>
void ReadValue(const std::string& root, const char* key, T& value) {
  JsonStore::Get().EnsureLoaded();
  const std::string full_key = root + CStrToString(key);
  const json::Value* v = JsonStore::Get().root().find(full_key);
  if (v == nullptr) return;
  if constexpr (std::is_same_v<T, bool>) {
    value = v->as_bool(value);
  } else if constexpr (std::is_same_v<T, int>) {
    value = v->as_int(value);
  } else if constexpr (std::is_same_v<T, float>) {
    value = v->as_float(value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    value = v->as_string();
  }
}

template <typename T>
void WriteValue(const std::string& root, const char* key, const T& value) {
  JsonStore::Get().EnsureLoaded();
  const std::string full_key = root + CStrToString(key);
  json::Value& target = JsonStore::Get().root().ensure(full_key);
  if constexpr (std::is_same_v<T, bool>) {
    target = json::Value(value);
  } else if constexpr (std::is_same_v<T, int>) {
    target = json::Value(value);
  } else if constexpr (std::is_same_v<T, float>) {
    target = json::Value(value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    target = json::Value(value);
  }
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

std::vector<ctl::InstanceKey> InstanceKeysForSections(
    const void* context, const std::string& section) {
  bool pass_indexed = false;
  for (const char* s : kPassSections) {
    if (section == s) { pass_indexed = true; break; }
  }
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

void LoadStore(Settings& settings, const std::string& prefix,
               ctl::PersistenceTarget target, bool include_ini) {
  JsonStore::Get().EnsureLoaded();
  ctl::ValueStore& store = feature::SettingsStore();
  store.ResetToDefaults(target);
  ctl::JsonConfigReader reader(JsonStore::Get().root());
  ctl::Load(store, reader, prefix, &InstanceKeysForSections, &store, target,
            ctl::PersistenceBackend::Json);
  if (include_ini) {
    ctl::ReshadeConfigReader ini_reader;
    ctl::Load(store, ini_reader, prefix, &InstanceKeysForSections, &store,
              target, ctl::PersistenceBackend::Ini);
  }
  SyncPassOrder(settings, true);
}

// Serialize the current ctl store as a self-contained data subtree, with
// pass ids compacted to i1..iN. The result is what gets written as a preset's
// "Data" and can be pasted into the root as "S" without breaking.
json::Value BuildPresetData() {
  WritePassOrder(feature::SettingsStore(), g_settings.pass_order);
  json::Value data = json::Value::MakeObject();
  {
    ctl::JsonConfigWriter writer(data);
    ctl::Save(feature::SettingsStore(), writer, "",
              &InstanceKeysForSections, &feature::SettingsStore(),
              ctl::PersistenceTarget::Profile,
              ctl::PersistenceBackend::Json);
  }
  if (json::Value* s = data.find("S")) {
    CompactPassKeysInTree(*s);
  }
  return data;
}

// Replace the current store from a preset Data subtree. Uses the loaded
// store's Pass Order to resolve instance keys, exactly as the normal load
// path does.
void ApplyPresetData(const json::Value& data) {
  ctl::ValueStore& store = feature::SettingsStore();
  store.ResetToDefaults(ctl::PersistenceTarget::Profile);
  ctl::JsonConfigReader reader(data);
  ctl::Load(store, reader, "", &InstanceKeysForSections, &store,
            ctl::PersistenceTarget::Profile,
            ctl::PersistenceBackend::Json);
  SyncPassOrder(g_settings, true);
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
  // No persistent counter. Pass ids on disk are compacted to i1..iN on every
  // save, so the next free id is one past the current maximum in memory.
  feature::pass::PassId next = 1;
  for (feature::pass::PassId id : g_settings.pass_order) {
    if (id >= next) next = id + 1;
  }
  return next;
}

void LoadSettings() {
  DLSSNR_PROFILE_CTL_L1("LoadSettings");
  // EnsureLoaded migrates the file to the current shape (array presets,
  // compact pass keys, no legacy counters) before anything is read.
  JsonStore::Get().EnsureLoaded();
  LoadSettingsValues(g_settings, "");
  LoadStore(g_settings, "", ctl::PersistenceTarget::Global, true);
  // Persist the migration and any state created by the load so the on-disk
  // form matches what the addon now saves.
  SaveSettings();
  {
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    const bool clone_resources = feature::SettingsStore().Read(
        feature::preferences::kSection, shared,
        feature::preferences::kCloneResources).as_bool;
    DlssGuides::Get().RequestCloneResources(clone_resources);
  }
  TRACE_SETTINGS(g_settings);
}

void SaveSettings() {
  DLSSNR_PROFILE_CTL_L1("SaveSettings");
  TRACE_SETTINGS(g_settings);
  JsonStore::Get().EnsureLoaded();
  json::Value& root = JsonStore::Get().root();

  WritePassOrder(feature::SettingsStore(), g_settings.pass_order);

  // Serialize into a scratch value so the pass keys can be compacted before
  // merging back. The live store keeps whatever ids the session is running
  // with; only the on-disk copy is renumbered.
  json::Value temp = json::Value::MakeObject();
  {
    ctl::JsonConfigWriter writer(temp);
    ctl::Save(feature::SettingsStore(), writer, "",
              &InstanceKeysForSections, &feature::SettingsStore(),
              ctl::PersistenceTarget::Global,
              ctl::PersistenceBackend::Json);
  }

  if (json::Value* temp_s = temp.find("S")) {
    CompactPassKeysInTree(*temp_s);
    root["S"] = std::move(*temp_s);
  } else {
    root.erase("S");
  }

  SaveSettingsValues(g_settings, "");
  root["Version"] = json::Value(kSettingsVersion);

  // Drop any top-level key the addon no longer writes, so legacy fields from
  // older builds (PassNextId, UserPresetOrder, UserPresetNextId, ...) cannot
  // survive forever.
  static const char* kKnown[] = { "Version", "Enabled", "S", "UserPreset" };
  std::set<std::string> known(std::begin(kKnown), std::end(kKnown));
  std::vector<std::string> drop;
  for (const auto& entry : root.object()) {
    if (known.find(entry.first) == known.end()) drop.push_back(entry.first);
  }
  for (const std::string& key : drop) root.erase(key);

  {
    ctl::ReshadeConfigWriter ini_writer;
    ctl::Save(feature::SettingsStore(), ini_writer, "",
              &InstanceKeysForSections, &feature::SettingsStore(),
              ctl::PersistenceTarget::Global,
              ctl::PersistenceBackend::Ini);
  }

  JsonStore::Get().Save();
}

void ErasePassKeys(feature::pass::PassId id) {
  ctl::ValueStore& store = feature::SettingsStore();
  const ctl::Schema& schema = store.schema();
  const ctl::InstanceKey key = feature::pass::PassKey(id);
  const std::string key_text = key.ToText();

  JsonStore::Get().EnsureLoaded();
  json::Value& root = JsonStore::Get().root();

  for (const char* section_name : kPassSections) {
    const ctl::Section* section = schema.FindSection(section_name);
    if (section == nullptr) continue;
    const std::string base =
        std::string("S.") + section_name + "." + key_text;
    for (const auto& setting : section->settings()) {
      if (!setting->meta().persist) continue;
      root.erase_path(base + "." + setting->meta().name);
    }
    store.EraseInstance(section_name, key);
  }
  JsonStore::Get().Save();
}

// ---------------------------------------------------------------------------
// Profiles: an ordered array under UserPreset. Identity is position; there is
// no external id or index counter. Each preset element is self-contained
// (Name, Enabled, Data.S) and can be cut or pasted freely.
// ---------------------------------------------------------------------------

namespace {

json::Value* GetPresetArray() {
  JsonStore::Get().EnsureLoaded();
  json::Value& root = JsonStore::Get().root();
  json::Value* presets = root.find("UserPreset");
  if (presets == nullptr) {
    root["UserPreset"] = json::Value::MakeArray();
    presets = root.find("UserPreset");
  }
  if (!presets->is_array()) {
    // Should have been handled by migration; guard against a hand-edited file.
    *presets = json::Value::MakeArray();
  }
  return presets;
}

std::string NormalizeProfileName(const char* name) {
  std::string clean = CStrToString(name);
  const auto first = clean.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = clean.find_last_not_of(" \t\r\n");
  clean = clean.substr(first, last - first + 1);
  if (clean.size() >= kSavedProfileNameCapacity) return {};
  return clean;
}

bool ProfileNameExists(const json::Value& arr, const std::string& name,
                       int skip_index) {
  const auto& v = arr.array();
  for (size_t i = 0; i < v.size(); ++i) {
    if (static_cast<int>(i) == skip_index) continue;
    if (const json::Value* n = v[i].find("Name")) {
      if (n->as_string() == name) return true;
    }
  }
  return false;
}

}  // namespace

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity) {
  json::Value* arr = GetPresetArray();
  const uint32_t count = static_cast<uint32_t>(arr->array().size());
  if (profiles != nullptr) {
    const uint32_t available = std::min(count, capacity);
    for (uint32_t i = 0; i < available; ++i) {
      profiles[i].id = i + 1;  // 1-based index into UserPreset
      std::string name;
      if (json::Value* n = arr->array()[i].find("Name")) {
        name = n->as_string();
      }
      std::snprintf(profiles[i].name, sizeof(profiles[i].name), "%s",
                    name.c_str());
    }
  }
  return count;
}

bool SaveCurrentAsProfile(const char* name) {
  const std::string clean = NormalizeProfileName(name);
  if (clean.empty()) return false;
  json::Value* arr = GetPresetArray();
  if (arr->array().size() >= kMaxSavedProfiles) return false;
  if (ProfileNameExists(*arr, clean, -1)) return false;

  json::Value preset = json::Value::MakeObject();
  preset["Name"] = json::Value(clean);
  preset["Enabled"] = json::Value(g_settings.enabled);
  preset["Data"] = BuildPresetData();

  arr->array().push_back(std::move(preset));
  JsonStore::Get().Save();
  return true;
}

bool RenameSavedProfile(ProfileId id, const char* name) {
  json::Value* arr = GetPresetArray();
  if (id < kFirstProfileId || id > arr->array().size()) return false;
  const std::string clean = NormalizeProfileName(name);
  if (clean.empty()) return false;
  const size_t idx = static_cast<size_t>(id - 1);
  if (ProfileNameExists(*arr, clean, static_cast<int>(idx))) return false;

  arr->array()[idx]["Name"] = json::Value(clean);
  JsonStore::Get().Save();
  return true;
}

bool SaveCurrentToProfile(ProfileId id) {
  json::Value* arr = GetPresetArray();
  if (id < kFirstProfileId || id > arr->array().size()) return false;
  const size_t idx = static_cast<size_t>(id - 1);

  json::Value& preset = arr->array()[idx];
  std::string name;
  if (json::Value* n = preset.find("Name")) name = n->as_string();

  preset = json::Value::MakeObject();
  preset["Name"] = json::Value(name);
  preset["Enabled"] = json::Value(g_settings.enabled);
  preset["Data"] = BuildPresetData();
  JsonStore::Get().Save();
  return true;
}

bool LoadSavedProfile(ProfileId id) {
  DLSSNR_PROFILE_CTL_L2("LoadSavedProfile");
  json::Value* arr = GetPresetArray();
  if (id < kFirstProfileId || id > arr->array().size()) return false;
  const size_t idx = static_cast<size_t>(id - 1);
  json::Value& preset = arr->array()[idx];

  if (json::Value* enabled_v = preset.find("Enabled")) {
    g_settings.enabled = enabled_v->as_bool(true);
  } else {
    g_settings.enabled = true;
  }

  json::Value* data = preset.find("Data");
  if (data == nullptr || !data->is_object()) return false;
  ApplyPresetData(*data);

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
  DLSSNR_PROFILE_CTL_L2("DeleteSavedProfile");
  json::Value* arr = GetPresetArray();
  if (id < kFirstProfileId || id > arr->array().size()) return false;
  const size_t idx = static_cast<size_t>(id - 1);
  auto& v = arr->array();
  v.erase(v.begin() + static_cast<std::ptrdiff_t>(idx));
  JsonStore::Get().Save();
  return true;
}

}  // namespace reshade_dlssnr

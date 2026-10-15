#include "ctl/values.h"

#include <algorithm>
#include <cstdio>

namespace reshade_dlssnr {
namespace ctl {

namespace {

uint32_t TotalSettingCount(const Schema& schema) {
  uint32_t total = 0;
  for (const auto& section : schema.sections()) {
    total += static_cast<uint32_t>(section->settings().size());
  }
  return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// InstanceKey
// ---------------------------------------------------------------------------

std::string InstanceKey::ToText() const {
  if (numeric_) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "i%lld",
                  static_cast<long long>(int_value_));
    return buffer;
  }
  // The prefix keeps an int key and a string key apart even when their text
  // would otherwise be identical.
  return "s" + string_value_;
}

bool InstanceKey::operator==(const InstanceKey& o) const {
  if (numeric_ != o.numeric_) return false;
  return numeric_ ? int_value_ == o.int_value_ : string_value_ == o.string_value_;
}

size_t InstanceKey::Hash::operator()(const InstanceKey& k) const {
  if (k.numeric()) {
    return std::hash<int64_t>{}(k.int_value()) ^ 0x9e3779b9u;
  }
  return std::hash<std::string>{}(k.string_value()) ^ 0x85ebca6bu;
}

// ---------------------------------------------------------------------------
// InstanceSet
// ---------------------------------------------------------------------------

Instance& InstanceSet::Create(const InstanceKey& key) {
  Instance instance;
  instance.key = key;
  const uint32_t setting_count =
      schema_ != nullptr ? TotalSettingCount(*schema_) : 0u;
  instance.states.resize(setting_count);
  instances_.push_back(std::move(instance));
  const uint32_t idx = static_cast<uint32_t>(instances_.size() - 1);
  index_[key] = idx;
  return instances_[idx];
}

Instance& InstanceSet::Get(const InstanceKey& key) {
  auto it = index_.find(key);
  if (it != index_.end()) return instances_[it->second];
  return Create(key);
}

Instance* InstanceSet::Find(const InstanceKey& key) {
  auto it = index_.find(key);
  if (it == index_.end()) return nullptr;
  return &instances_[it->second];
}

const Instance* InstanceSet::Find(const InstanceKey& key) const {
  auto it = index_.find(key);
  if (it == index_.end()) return nullptr;
  return &instances_[it->second];
}

// ---------------------------------------------------------------------------
// ValueStore
// ---------------------------------------------------------------------------

void ValueStore::Init(const Schema& schema) {
  schema_ = &schema;
  section_sets_.clear();
  section_offset_.clear();
  section_sets_.resize(schema.section_count());
  section_offset_.resize(schema.section_count());

  uint32_t offset = 0;
  for (uint32_t i = 0; i < schema.section_count(); ++i) {
    section_sets_[i].set_schema(&schema);
    section_offset_[i] = offset;
    offset += static_cast<uint32_t>(schema.sections()[i]->settings().size());
  }
  setting_count_ = offset;
}

InstanceSet& ValueStore::SectionSet(const std::string& name) {
  const Section* section = schema_->FindSection(name);
  if (section == nullptr) {
    static InstanceSet empty;
    return empty;
  }
  return section_sets_[section->index()];
}

const InstanceSet& ValueStore::SectionSet(const std::string& name) const {
  const Section* section = schema_->FindSection(name);
  if (section == nullptr) {
    static InstanceSet empty;
    return empty;
  }
  return section_sets_[section->index()];
}

InstanceSet& ValueStore::SectionSet(const Section& section) {
  return section_sets_[section.index()];
}

const InstanceSet& ValueStore::SectionSet(const Section& section) const {
  return section_sets_[section.index()];
}

const Setting* ValueStore::SettingAt(uint32_t setting_id) const {
  if (setting_id >= setting_count_) return nullptr;
  uint32_t section_index = 0;
  for (uint32_t i = 0; i < section_offset_.size(); ++i) {
    const uint32_t count =
        static_cast<uint32_t>(schema_->sections()[i]->settings().size());
    if (setting_id < section_offset_[i] + count) {
      section_index = i;
      break;
    }
  }
  const uint32_t local = setting_id - section_offset_[section_index];
  return schema_->sections()[section_index]->settings()[local].get();
}

uint32_t ValueStore::SectionIndexOfSetting(uint32_t setting_id) const {
  for (uint32_t i = 0; i < section_offset_.size(); ++i) {
    const uint32_t count =
        static_cast<uint32_t>(schema_->sections()[i]->settings().size());
    if (setting_id < section_offset_[i] + count) return i;
  }
  return 0;
}

bool ValueStore::FindSettingId(const std::string& section,
                               const std::string& setting,
                               uint32_t* out) const {
  const Section* s = schema_->FindSection(section);
  if (s == nullptr) return false;
  const auto& settings = s->settings();
  for (uint32_t i = 0; i < settings.size(); ++i) {
    if (settings[i]->meta().name == setting) {
      if (out != nullptr) *out = section_offset_[s->index()] + i;
      return true;
    }
  }
  return false;
}

Value ValueStore::Resolve(const Instance& instance, uint32_t setting_id) const {
  const Setting* setting = SettingAt(setting_id);
  if (setting == nullptr) return Value();
  const SettingState& state = instance.states[setting_id];
  return state.has_value ? state.value : setting->DefaultValue();
}

Value ValueStore::Read(const std::string& section, const InstanceKey& key,
                       const std::string& setting) const {
  uint32_t id = 0;
  if (!FindSettingId(section, setting, &id)) return Value();
  InstanceSet& set = const_cast<ValueStore*>(this)->SectionSet(section);
  Instance* instance = set.Find(key);
  if (instance == nullptr) {
    const Setting* s = SettingAt(id);
    return s != nullptr ? s->DefaultValue() : Value();
  }
  return Resolve(*instance, id);
}

bool ValueStore::Write(const std::string& section, const InstanceKey& key,
                       const std::string& setting, const Value& value,
                       bool from_user) {
  uint32_t id = 0;
  if (!FindSettingId(section, setting, &id)) return false;
  const Setting* declared = SettingAt(id);
  if (declared == nullptr) return false;

  Value stored = value;
  declared->Clamp(stored);

  InstanceSet& set = SectionSet(section);
  Instance& instance = set.Get(key);
  instance.touched = true;
  SettingState& state = instance.states[id];

  const Value previous = state.has_value ? state.value : Value();
  state.value = stored;
  state.has_value = true;
  state.changed_by_user = from_user;
  state.changed_programmatically = !from_user;

  if (previous != stored) MarkChanged();
  return true;
}

bool ValueStore::ResetToDefault(const std::string& section, const InstanceKey& key,
                                const std::string& setting) {
  uint32_t id = 0;
  if (!FindSettingId(section, setting, &id)) return false;
  const Setting* declared = SettingAt(id);
  if (declared == nullptr) return false;

  InstanceSet& set = SectionSet(section);
  Instance& instance = set.Get(key);
  instance.touched = true;
  SettingState& state = instance.states[id];
  state.value = declared->DefaultValue();
  state.has_value = true;
  state.changed_by_user = false;
  state.changed_programmatically = true;
  MarkChanged();
  return true;
}

void ValueStore::ResetSectionToDefault(const std::string& section,
                                       const InstanceKey& key) {
  const Section* s = schema_->FindSection(section);
  if (s == nullptr) return;
  for (const auto& setting : s->settings()) {
    ResetToDefault(section, key, setting->meta().name);
  }
}

void ValueStore::ResetToDefaults() {
  for (InstanceSet& set : section_sets_) {
    for (Instance& instance : set.instances()) {
      for (SettingState& state : instance.states) {
        state.has_value = false;
        state.value = Value();
        state.changed_by_user = false;
        state.changed_programmatically = false;
      }
    }
  }
  MarkChanged();
}

void ValueStore::MarkChanged() { dirty_ = true; }

}  // namespace ctl
}  // namespace reshade_dlssnr

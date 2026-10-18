// ctl instance values: one value set per instance, owned outright.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ctl/schema.h"
#include "ctl/types.h"

namespace reshade_dlssnr {
namespace ctl {

// ---------------------------------------------------------------------------
// Key
// ---------------------------------------------------------------------------

// An opaque instance key. The framework assigns it no meaning whatsoever --
// it is not required to be an index, monotonic, contiguous, or numeric. Two
// key flavours are supported because callers naturally have one or the other;
// the framework normalizes both into a single comparison value.
class InstanceKey {
 public:
  static InstanceKey FromInt(int64_t v) {
    InstanceKey k;
    k.numeric_ = true;
    k.int_value_ = v;
    return k;
  }
  static InstanceKey FromString(std::string v) {
    InstanceKey k;
    k.numeric_ = false;
    k.string_value_ = std::move(v);
    return k;
  }

  bool numeric() const { return numeric_; }
  int64_t int_value() const { return int_value_; }
  const std::string& string_value() const { return string_value_; }

  // A stable textual form, used for persistence. Chosen so an int key and a
  // string key can never collide in the config.
  std::string ToText() const;

  bool operator==(const InstanceKey& o) const;
  bool operator!=(const InstanceKey& o) const { return !(*this == o); }

  struct Hash {
    size_t operator()(const InstanceKey& k) const;
  };

 private:
  bool numeric_ = true;
  int64_t int_value_ = 0;
  std::string string_value_;
};

// ---------------------------------------------------------------------------
// Per-setting state
// ---------------------------------------------------------------------------

struct SettingState {
  Value value;
  bool has_value = false;
  bool changed_by_user = false;
  bool changed_programmatically = false;
};

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

// One value set for one key. Created lazily on first access.
struct Instance {
  InstanceKey key;
  std::vector<SettingState> states;
  bool touched = false;
};

// ---------------------------------------------------------------------------
// InstanceSet
// ---------------------------------------------------------------------------

// All instances of one instanced (or single) section, addressed by key.
//
// A `Single` section uses exactly one instance, created on first access. The
// code path is identical to an instanced section; only the number of live
// instances differs, so single and instanced sections need no special-casing
// anywhere else in the framework.
class InstanceSet {
 public:
  InstanceSet() = default;
  explicit InstanceSet(const Schema* schema) : schema_(schema) {}

  void set_schema(const Schema* schema) { schema_ = schema; }

  // Get or lazily create the instance for `key`.
  Instance& Get(const InstanceKey& key);
  // Non-creating lookup. Both overloads exist so a const InstanceSet still
  // yields a const Instance.
  Instance* Find(const InstanceKey& key);
  const Instance* Find(const InstanceKey& key) const;
  bool Erase(const InstanceKey& key);
  void Retain(const std::vector<InstanceKey>& keys);

  std::vector<Instance>& instances() { return instances_; }
  const std::vector<Instance>& instances() const { return instances_; }
  const InstanceKey& single_key() const { return single_key_; }

 private:
  Instance& Create(const InstanceKey& key);

  const Schema* schema_ = nullptr;
  std::vector<Instance> instances_;
  std::unordered_map<InstanceKey, uint32_t, InstanceKey::Hash> index_;
  InstanceKey single_key_ = InstanceKey::FromInt(0);
};

// ---------------------------------------------------------------------------
// ValueStore
// ---------------------------------------------------------------------------

// The whole live settings state: one InstanceSet per instanced section and
// one shared InstanceSet for all single-instance sections.
class ValueStore {
 public:
  ValueStore() = default;
  explicit ValueStore(const Schema& schema) { Init(schema); }

  void Init(const Schema& schema);
  const Schema& schema() const { return *schema_; }

  // Section access. Both kinds return an InstanceSet; a single-instance
  // section simply has one instance.
  //
  // The name is deliberately not `Section`: a member function named after
  // the type it takes would hide that type everywhere inside this class.
  InstanceSet& SectionSet(const std::string& name);
  const InstanceSet& SectionSet(const std::string& name) const;
  InstanceSet& SectionSet(const Section& section);
  const InstanceSet& SectionSet(const Section& section) const;
  bool EraseInstance(const std::string& section, const InstanceKey& key);

  // Resolve a setting for an instance: its own value, or the declared
  // default when nothing has been written yet.
  Value Resolve(const Instance& instance, uint32_t setting_id) const;

  // Programmatic read. Returns the resolved value. Never touches the UI.
  Value Read(const std::string& section, const InstanceKey& key,
             const std::string& setting) const;

  // Programmatic write. Stores an override on the instance and propagates to
  // non-overriding descendants. Returns false when the setting does not
  // exist or is not writable.
  bool Write(const std::string& section, const InstanceKey& key,
             const std::string& setting, const Value& value,
             bool from_user = false);



  // Reset one setting to its declared default, per instance. Distinct from
  // Revert: this clears the override and writes the default as an override,
  // preserving "the user set this to the default" where that matters.
  bool ResetToDefault(const std::string& section, const InstanceKey& key,
                      const std::string& setting);

  // Reset every setting in a section to its declared default, per instance.
  void ResetSectionToDefault(const std::string& section, const InstanceKey& key);

  // Drop every override in the whole store, on every instance of every
  // section. Used before a bulk load, so that what a file describes replaces
  // what the store holds rather than merging into it -- a profile load must
  // produce exactly the profile's state, not the current state plus the
  // profile's overrides.
  //
  // Instances survive: which keys exist is not this store's decision to make.
  void ResetToDefaults(PersistenceTarget target = PersistenceTarget::Global);

  // Resolve the global setting id (index into Instance::states) of a setting.
  bool FindSettingId(const std::string& section, const std::string& setting,
                     uint32_t* out) const;

  // Where the global setting id space is laid out: one entry per (section,
  // setting) pair, in section order then declaration order.
  const Setting* SettingAt(uint32_t setting_id) const;
  uint32_t setting_count() const { return setting_count_; }

  // First global setting id of a section. Lets a caller walk a section's
  // settings without recomputing the layout.
  uint32_t SettingOffsetOf(const Section& section) const {
    return section_offset_[section.index()];
  }

  bool dirty() const { return dirty_; }
  void clear_dirty() { dirty_ = false; }

 private:
  const Schema* schema_ = nullptr;
  std::vector<InstanceSet> section_sets_;
  std::vector<uint32_t> section_offset_;  // first setting id of each section
  uint32_t setting_count_ = 0;
  bool dirty_ = false;

  void MarkChanged();
};

}  // namespace ctl
}  // namespace reshade_dlssnr

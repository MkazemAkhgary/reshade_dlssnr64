#include "ctl/serialize.h"
#include "profiling/profile.h"

#include <string>
#include <vector>

namespace reshade_dlssnr {
namespace ctl {

namespace {

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  return a + "." + b;
}

std::string SectionPrefix(const std::string& prefix,
                          const std::string& section) {
  return Join(prefix, "S." + section);
}

// A single-instance section lives directly under its section prefix. An
// instanced section nests one level deeper, under its instance key.
std::string InstancePrefix(const std::string& prefix, const std::string& section,
                           bool instanced, const InstanceKey& key) {
  const std::string base = SectionPrefix(prefix, section);
  if (!instanced) return base;
  return Join(base, key.ToText());
}

void WriteValue(KeyValueWriter& writer, const std::string& key,
                const Setting& setting, const Value& value) {
  switch (value.kind) {
    case Value::Kind::Float:
      writer.WriteFloat(key, value.as_float);
      break;
    case Value::Kind::Int:
    case Value::Kind::Enum:
      writer.WriteInt(key, value.as_int);
      break;
    case Value::Kind::Bool:
      writer.WriteBool(key, value.as_bool);
      break;
    case Value::Kind::String:
    case Value::Kind::IntList:
      // Empty values are written, not skipped: clearing a setting is distinct
      // from the absence of a persisted override.
      writer.WriteString(key, setting.ToString(value));
      break;
  }
}

bool ReadValue(KeyValueReader& reader, const std::string& key,
               const Setting& setting, Value* out) {
  switch (out->kind) {
    case Value::Kind::Float: {
      float v = 0.0f;
      if (!reader.ReadFloat(key, &v)) return false;
      out->as_float = v;
      return true;
    }
    case Value::Kind::Int:
    case Value::Kind::Enum: {
      int v = 0;
      if (!reader.ReadInt(key, &v)) return false;
      out->as_int = v;
      return true;
    }
    case Value::Kind::Bool: {
      bool v = false;
      if (!reader.ReadBool(key, &v)) return false;
      out->as_bool = v;
      return true;
    }
    // Returns false only when the key is absent. An empty string is a value
    // the file can hold ("the user cleared it"), not an absent key, so the
    // presence test and the content are separate.
    case Value::Kind::String:
    case Value::Kind::IntList: {
      std::string text;
      if (!reader.ReadString(key, &text)) return false;
      return setting.FromString(text, *out);
    }
  }
  return false;
}

bool ShouldPersist(const Setting& setting, PersistenceTarget target,
                   PersistenceBackend backend) {
  if (!setting.meta().persist) return false;
  if (setting.meta().backend != backend) return false;
  return target == PersistenceTarget::Global ||
         setting.meta().persist_in_profiles;
}

Value ValueToWrite(const Setting& setting, const SettingState& state) {
  return state.has_value ? state.value : setting.DefaultValue();
}

void SaveInstance(ValueStore& store, KeyValueWriter& writer,
                  const std::string& prefix, const Section& section,
                  const Instance& instance, bool instanced,
                  PersistenceTarget target,
                  PersistenceBackend backend) {
  const std::string base =
      InstancePrefix(prefix, section.name(), instanced, instance.key);
  const uint32_t offset = store.SettingOffsetOf(section);
  for (uint32_t i = 0; i < section.settings().size(); ++i) {
    const Setting& setting = *section.settings()[i];
    const SettingState& state = instance.states[offset + i];
    if (!ShouldPersist(setting, target, backend)) continue;
    WriteValue(writer, Join(base, setting.meta().name), setting,
               ValueToWrite(setting, state));
  }
}

}  // namespace

void Save(ValueStore& store, KeyValueWriter& writer,
          const std::string& prefix, const InstanceKeySource& source,
          const void* context, PersistenceTarget target,
          PersistenceBackend backend) {
  DLSSNR_PROFILE_CTL_L2("ctl::Save");

  const Schema& schema = store.schema();
  for (const auto& section_ptr : schema.sections()) {
    const Section& section = *section_ptr;

    if (!section.instanced()) {
      // Single-instance sections: one block, no key.
      InstanceSet& set = store.SectionSet(section);
      const Instance& instance = set.Get(set.single_key());
      SaveInstance(store, writer, prefix, section, instance, false, target,
                   backend);
      continue;
    }

    // Instanced sections: use the consumer's declared key set when supplied.
    InstanceSet& set = store.SectionSet(section);
    if (source == nullptr) {
      for (const Instance& instance : set.instances()) {
        SaveInstance(store, writer, prefix, section, instance, true, target,
                     backend);
      }
    } else {
      const std::vector<InstanceKey> keys = source(context, section.name());
      for (const InstanceKey& key : keys) {
        // Declared keys are part of the save contract even if a collapsed UI
        // has not touched that section yet; Get creates the default instance.
        const Instance& instance = set.Get(key);
        SaveInstance(store, writer, prefix, section, instance, true, target,
                     backend);
      }
    }
  }
}

void Delete(ValueStore& store, KeyValueWriter& writer,
            const std::string& prefix, const InstanceKeySource& source,
            const void* context, PersistenceTarget target,
            PersistenceBackend backend) {
  DLSSNR_PROFILE_CTL_L2("ctl::Delete");

  const Schema& schema = store.schema();
  for (const auto& section_ptr : schema.sections()) {
    const Section& section = *section_ptr;
    if (!section.instanced()) {
      const std::string base = InstancePrefix(prefix, section.name(), false, {});
      for (const auto& setting_ptr : section.settings()) {
        const Setting& setting = *setting_ptr;
        if (!ShouldPersist(setting, target, backend)) continue;
        writer.Erase(Join(base, setting.meta().name));
      }
      continue;
    }

    if (source == nullptr) continue;
    const std::vector<InstanceKey> keys = source(context, section.name());
    for (const InstanceKey& key : keys) {
      const std::string base = InstancePrefix(prefix, section.name(), true, key);
      for (const auto& setting_ptr : section.settings()) {
        const Setting& setting = *setting_ptr;
        if (!ShouldPersist(setting, target, backend)) continue;
        writer.Erase(Join(base, setting.meta().name));
      }
    }
  }
}

void Load(ValueStore& store, KeyValueReader& reader,
          const std::string& prefix, const InstanceKeySource& source,
          const void* context, PersistenceTarget target,
          PersistenceBackend backend) {
  DLSSNR_PROFILE_CTL_L2("ctl::Load");

  const Schema& schema = store.schema();
  for (const auto& section_ptr : schema.sections()) {
    const Section& section = *section_ptr;

    if (!section.instanced()) {
      InstanceSet& set = store.SectionSet(section.name());
      Instance& instance = set.Get(set.single_key());
      const std::string base = InstancePrefix(prefix, section.name(), false, {});
      const uint32_t offset = store.SettingOffsetOf(section);
      for (uint32_t i = 0; i < section.settings().size(); ++i) {
        const Setting& setting = *section.settings()[i];
        if (!ShouldPersist(setting, target, backend)) continue;
        SettingState& state = instance.states[offset + i];
        Value candidate = setting.DefaultValue();
        if (!ReadValue(reader, Join(base, setting.meta().name), setting,
                       &candidate)) {
          continue;  // absent: keep the declared default
        }
        setting.Clamp(candidate);
        state.value = candidate;
        state.has_value = true;
      }
      continue;
    }

    // Instanced: the set of live instances is whatever the caller declares,
    // not whatever the config happens to mention. The caller owns instance
    // count, so it hands the keys over; each one is created on contact and
    // then filled from the config, defaulting anything the config omits.
    InstanceSet& set = store.SectionSet(section.name());
    const uint32_t offset = store.SettingOffsetOf(section);

    const std::vector<InstanceKey> keys =
        source != nullptr ? source(context, section.name())
                          : std::vector<InstanceKey>();
    set.Retain(keys);

    for (const InstanceKey& key : keys) {
      Instance& instance = set.Get(key);
      const std::string base =
          InstancePrefix(prefix, section.name(), true, key);
      for (uint32_t i = 0; i < section.settings().size(); ++i) {
        const Setting& setting = *section.settings()[i];
        if (!ShouldPersist(setting, target, backend)) continue;
        SettingState& state = instance.states[offset + i];
        Value candidate = setting.DefaultValue();
        if (!ReadValue(reader, Join(base, setting.meta().name), setting,
                       &candidate)) {
          continue;  // absent: keep the declared default
        }
        setting.Clamp(candidate);
        state.value = candidate;
        state.has_value = true;
      }
    }
  }
}

}  // namespace ctl
}  // namespace reshade_dlssnr

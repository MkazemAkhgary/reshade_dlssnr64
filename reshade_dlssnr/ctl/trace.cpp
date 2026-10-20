#include "ctl/trace.h"
#include "profiling/profile.h"

#include <algorithm>
#include <unordered_map>

namespace reshade_dlssnr {
namespace ctl {

std::vector<TraceRecorder::Entry> TraceRecorder::Capture(
    ValueStore& store) const {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Capture");
  std::vector<TraceRecorder::Entry> entries;
  const Schema& schema = store.schema();
  if (!schema.built()) return entries;

  for (const auto& section_ptr : schema.sections()) {
    const Section& section = *section_ptr;
    InstanceSet& instances = store.SectionSet(section);
    if (!section.instanced()) {
      instances.Get(instances.single_key());
    }

    const uint32_t offset = store.SettingOffsetOf(section);
    for (const Instance& instance : instances.instances()) {
      const std::string instance_prefix = section.instanced()
          ? section.name() + "[" + instance.key.ToText() + "]."
          : section.name() + ".";
      for (uint32_t i = 0; i < section.settings().size(); ++i) {
        const Setting& setting = *section.settings()[i];
        const Value value = store.Resolve(instance, offset + i);
        entries.push_back(TraceRecorder::Entry{
            instance_prefix + setting.meta().name, setting.ToString(value)});
      }
    }
  }

  std::sort(entries.begin(), entries.end(),
            [](const TraceRecorder::Entry& a, const TraceRecorder::Entry& b) {
              return a.name < b.name;
            });
  return entries;
}

void TraceRecorder::Record(ValueStore& store, TraceSink sink, void* context) {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Record");
  if (sink == nullptr) return;

  const std::vector<Entry> current = Capture(store);
  if (!has_previous_) {
    for (const Entry& entry : current) {
      sink(context, TraceChange{TraceChangeKind::Initial, entry.name, {},
                                entry.value});
    }
    previous_ = current;
    has_previous_ = true;
    return;
  }

  std::unordered_map<std::string, std::string> previous_values;
  previous_values.reserve(previous_.size());
  for (const Entry& entry : previous_)
    previous_values.emplace(entry.name, entry.value);

  std::unordered_map<std::string, std::string> current_values;
  current_values.reserve(current.size());
  for (const Entry& entry : current) {
    current_values.emplace(entry.name, entry.value);
    const auto previous = previous_values.find(entry.name);
    if (previous == previous_values.end()) {
      sink(context, TraceChange{TraceChangeKind::Initial, entry.name, {},
                                entry.value});
    } else if (previous->second != entry.value) {
      sink(context, TraceChange{TraceChangeKind::Changed, entry.name,
                                previous->second, entry.value});
    }
  }

  for (const Entry& entry : previous_) {
    if (current_values.find(entry.name) == current_values.end()) {
      sink(context, TraceChange{TraceChangeKind::Removed, entry.name,
                                entry.value, {}});
    }
  }

  previous_ = current;
}

void TraceRecorder::Reset() {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Reset");
  previous_.clear();
  has_previous_ = false;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

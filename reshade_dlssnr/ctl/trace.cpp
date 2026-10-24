#include "ctl/trace.h"
#include "profiling/profile.h"

#include <algorithm>

namespace reshade_dlssnr {
namespace ctl {

void TraceRecorder::Capture(ValueStore& store, std::vector<Entry>& out) {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Capture");
  const Schema& schema = store.schema();
  if (!schema.built()) {
    out.resize(0);
    return;
  }

  // Slots are filled in place so name/value buffers survive across calls.
  size_t used = 0;
  auto slot = [&out, &used]() -> Entry& {
    if (used == out.size()) out.emplace_back();
    return out[used++];
  };

  for (const auto& section_ptr : schema.sections()) {
    const Section& section = *section_ptr;
    InstanceSet& instances = store.SectionSet(section);
    if (!section.instanced()) {
      instances.Get(instances.single_key());
    }

    const uint32_t offset = store.SettingOffsetOf(section);
    for (const Instance& instance : instances.instances()) {
      for (uint32_t i = 0; i < section.settings().size(); ++i) {
        const Setting& setting = *section.settings()[i];
        Entry& entry = slot();
        entry.name.assign(section.name());
        if (section.instanced()) {
          entry.name += '[';
          entry.name += instance.key.ToText();
          entry.name += "].";
        } else {
          entry.name += '.';
        }
        entry.name += setting.meta().name;
        entry.value = setting.ToString(store.Resolve(instance, offset + i));
      }
    }
  }
  out.resize(used);

  std::sort(out.begin(), out.end(),
            [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

void TraceRecorder::Record(ValueStore& store, TraceSink sink, void* context) {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Record");
  if (sink == nullptr) return;

  Capture(store, current_);
  if (!has_previous_) {
    for (const Entry& entry : current_) {
      sink(context, TraceChange{TraceChangeKind::Initial, entry.name, {},
                                entry.value});
    }
    previous_.swap(current_);
    has_previous_ = true;
    return;
  }

  // Both sides are sorted by name, so one merge pass finds every add, change,
  // and removal without building lookup tables.
  size_t prev = 0;
  size_t cur = 0;
  while (prev < previous_.size() && cur < current_.size()) {
    const Entry& p = previous_[prev];
    const Entry& c = current_[cur];
    if (p.name < c.name) {
      sink(context, TraceChange{TraceChangeKind::Removed, p.name, p.value, {}});
      ++prev;
    } else if (c.name < p.name) {
      sink(context, TraceChange{TraceChangeKind::Initial, c.name, {}, c.value});
      ++cur;
    } else {
      if (p.value != c.value) {
        sink(context, TraceChange{TraceChangeKind::Changed, c.name, p.value,
                                  c.value});
      }
      ++prev;
      ++cur;
    }
  }
  for (; prev < previous_.size(); ++prev) {
    const Entry& p = previous_[prev];
    sink(context, TraceChange{TraceChangeKind::Removed, p.name, p.value, {}});
  }
  for (; cur < current_.size(); ++cur) {
    const Entry& c = current_[cur];
    sink(context, TraceChange{TraceChangeKind::Initial, c.name, {}, c.value});
  }

  previous_.swap(current_);
}

void TraceRecorder::Reset() {
  DLSSNR_PROFILE_CTL_L3("TraceRecorder::Reset");
  previous_.clear();
  has_previous_ = false;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

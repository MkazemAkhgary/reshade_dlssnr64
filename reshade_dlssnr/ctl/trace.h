// Generic change tracing for registered ctl settings.
#pragma once

#include <string>
#include <vector>

#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

enum class TraceChangeKind { Initial, Changed, Removed };

struct TraceChange {
  TraceChangeKind kind = TraceChangeKind::Initial;
  std::string name;
  std::string previous_value;
  std::string value;
};

using TraceSink = void (*)(void* context, const TraceChange& change);

// Captures every declared setting from the live store, including default-valued
// settings. Section, instance key, and setting name form the stable trace key.
class TraceRecorder {
 public:
  void Record(ValueStore& store, TraceSink sink, void* context = nullptr);
  void Reset();

 private:
  struct Entry {
    std::string name;
    std::string value;
  };

  // Fills `out` from the live store, reusing its existing string buffers.
  void Capture(ValueStore& store, std::vector<Entry>& out);
  std::vector<Entry> current_;
  std::vector<Entry> previous_;
  bool has_previous_ = false;
};

}  // namespace ctl
}  // namespace reshade_dlssnr

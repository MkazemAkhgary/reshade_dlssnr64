#include "feature/pass/menu.h"

#include <cstdint>

#include "ctl/menu.h"
#include "ctl/values.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "state/settings.h"

#define LOC_NAMESPACE "pass"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

namespace {

// The direction is the command's declaration-time payload, so one pair of
// functions serves both commands.
constexpr int kPrevious = -1;
constexpr int kNext = 1;

int Direction(const void* context) { return *static_cast<const int*>(context); }

// The instance one position away from `key` in the tab order. A key that is
// not a pass -- every single-instance section, for one -- has no neighbour.
bool Neighbour(const ctl::ValueStore& store, const ctl::InstanceKey& key,
               int direction, ctl::InstanceKey* out) {
  if (!key.numeric()) return false;
  const PassOrder order = ReadPassOrder(store);
  for (size_t i = 0; i < order.size(); ++i) {
    if (static_cast<int64_t>(order[i]) != key.int_value()) continue;
    const int64_t next = static_cast<int64_t>(i) + direction;
    if (next < 0 || next >= static_cast<int64_t>(order.size())) return false;
    *out = PassKey(order[static_cast<size_t>(next)]);
    return true;
  }
  return false;
}

bool HasNeighbour(const void* context, const ctl::ValueStore& store,
                  const ctl::MenuTarget& target) {
  ctl::InstanceKey unused;
  return Neighbour(store, target.key, Direction(context), &unused);
}

bool CopyFromNeighbour(const void* context, ctl::ValueStore& store,
                       const ctl::MenuTarget& target) {
  ctl::InstanceKey source;
  if (!Neighbour(store, target.key, Direction(context), &source)) return false;
  return ctl::CopyFromInstance(store, target, source);
}

}  // namespace

void RegisterPassMenuCommands(ctl::MenuCommandRegistry& registry) {
  registry.Add({.name = "pass.copy_from_previous",
                .group = "pass",
                .label = LOC_TEXT("menu.copy_from_previous",
                                   "Copy from previous pass"),
                .visible = &HasNeighbour,
                .execute = &CopyFromNeighbour,
                .context = &kPrevious});
  registry.Add({.name = "pass.copy_from_next",
                .group = "pass",
                .label = LOC_TEXT("menu.copy_from_next",
                                   "Copy from next pass"),
                .visible = &HasNeighbour,
                .execute = &CopyFromNeighbour,
                .context = &kNext});
}

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

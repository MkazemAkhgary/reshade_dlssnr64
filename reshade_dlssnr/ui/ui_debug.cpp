#include "ui/ui_sections.h"

#include <vector>

#include "debug/debug_view.h"
#include "state/settings.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {
namespace {

struct DebugViewChoices {
  std::vector<const char*> names;
  std::vector<const char*> tips;
};

const DebugViewChoices& GetDebugViewChoices() {
  static const DebugViewChoices choices = [] {
    DebugViewChoices result;
    result.names.reserve(16);
    result.tips.reserve(16);

    result.names.push_back("Off");
    result.tips.push_back(
        "Disable the debug view and show the normal composed output.");

    for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
      result.names.push_back(kDebugViews[i].name);
      result.tips.push_back(kDebugViews[i].tooltip);
    }
    return result;
  }();
  return choices;
}

}  // namespace

void DrawSharedDebugView(Settings& settings, bool& changed) {
  const DebugViewChoices& choices = GetDebugViewChoices();
  changed |= DrawComboRow(
      "debug_view", "Debug View",
      "Replace the final image with an internal buffer, for inspecting "
      "the pipeline.\n"
      "The view is rendered by whichever pass is currently selected in "
      "the tabs below, and only takes effect while that pass is enabled.",
      &settings.debug_view, choices.names.data(), choices.tips.data(),
      static_cast<int>(choices.names.size()));
}

}  // namespace reshade_dlssnr

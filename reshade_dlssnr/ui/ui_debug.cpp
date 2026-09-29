#include "ui/ui_sections.h"

#include <vector>

#include "debug_view.h"
#include "settings.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

void DrawSharedDebugView(Settings& settings, bool& changed) {
  std::vector<const char*> names;
  std::vector<const char*> tips;
  names.reserve(8);
  tips.reserve(8);

  names.push_back("Off");
  tips.push_back(
      "Disable the debug view and show the normal composed output.");

  for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
    names.push_back(kDebugViews[i].name);
    tips.push_back(kDebugViews[i].tooltip);
  }

  changed |= DrawComboRow(
      "debug_view", "Debug View",
      "Replace the final image with an internal buffer, for inspecting "
      "the pipeline.\n"
      "The view is rendered by whichever pass is currently selected in "
      "the tabs below, and only takes effect while that pass is enabled.",
      &settings.debug_view, names.data(), tips.data(),
      static_cast<int>(names.size()));
}

}  // namespace reshade_dlssnr

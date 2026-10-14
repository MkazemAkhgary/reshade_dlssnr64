// Debug view schema, declared inline.
//
// The item list lives in a runtime table. Rather than declaring the enum empty
// and filling it in later, the setting takes an item provider at declaration
// time -- so everything the setting needs is present the moment it is declared,
// and there is no install step, no post-Build schema mutation, and no cast.

#include <imgui.h>

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "debug/debug_view.h"
#include "feature/debug_log/settings.h"

namespace reshade_dlssnr {
namespace feature {

namespace {


// The provider. It reads the table the debug layer already owns, so the names
// and tooltips have exactly one definition. Item 0 is "Off": the value
// convention is 0 = off, N = kDebugViews[N-1].
std::vector<ctl::EnumSetting::Item> DebugViewItems() {
  std::vector<ctl::EnumSetting::Item> items;
  items.push_back({"Off",
                   "Disable the debug view and show the normal composed "
                   "output."});
  for (const DebugViewDef* view = kDebugViews; view->name != nullptr; ++view) {
    items.push_back({view->name, view->tooltip});
  }
  return items;
}

}  // namespace

void RegisterDebugLogSchema(ctl::Schema& schema) {
  ctl::Section& debug = schema.AddSection(debug_log::kSection, ctl::SectionKind::Single);

  debug
      .Add<ctl::EnumSetting>(debug_log::kDebugView, "Debug View", 0)
      .Tooltip("Replace the final image with an internal buffer, for "
               "inspecting the pipeline.")
      .Items(&DebugViewItems);

  // The hint that the view belongs to the selected pass is not a setting.
  debug.AddHookAfter(debug_log::kDebugView, "DebugViewHint",
                     [](const void*, ctl::ValueStore&, const ctl::DrawOptions&,
                        const std::string&, const ctl::InstanceKey&, bool&) {
                       ImGui::TextDisabled(
                           "The view is rendered by whichever pass is "
                           "currently selected in the tabs below, and only "
                           "takes effect while that pass is enabled.");
                       return false;
                     });
}

}  // namespace feature
}  // namespace reshade_dlssnr

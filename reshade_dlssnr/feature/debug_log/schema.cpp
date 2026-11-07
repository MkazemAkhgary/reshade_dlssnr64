// Debug view schema, declared inline.
//
// The item list lives in a runtime table. Rather than declaring the enum empty
// and filling it in later, the setting takes an item provider at declaration
// time -- so everything the setting needs is present the moment it is declared,
// and there is no install step, no post-Build schema mutation, and no cast.

#include <imgui.h>

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "debug/debug_view.h"
#include "feature/debug_log/settings.h"

#define LOC_NAMESPACE "debug"

namespace reshade_dlssnr {
namespace feature {

namespace {


// The provider. It reads the table the debug layer already owns, so the names
// and tooltips have exactly one definition. Item 0 is "Off": the value
// convention is 0 = off, N = kDebugViews[N-1].
std::vector<ctl::EnumSetting::Item> DebugViewItems() {
  DLSSNR_PROFILE_CTL_L3("DebugViewItems");
  std::vector<ctl::EnumSetting::Item> items;
  items.push_back({LOC_TEXT("view.off", "Off"),
                   LOC_TEXT("view.off.tip",
                            "Disable the debug view and show the normal composed "
                            "output.")});
  for (const DebugViewDef* view = kDebugViews; !view->name.empty(); ++view) {
    items.push_back({view->name, view->tooltip});
  }
  return items;
}

}  // namespace

void RegisterDebugLogSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterDebugLogSchema");
  ctl::Section& debug = schema.AddSection(debug_log::kSection, ctl::SectionKind::Single);
  debug.Title(LOC_TEXT("title", "Debug"));
  debug.DefaultPersistenceBackend(ctl::PersistenceBackend::Ini);

  debug
      .Add<ctl::EnumSetting>(debug_log::kDebugView,
                             LOC_TEXT("view", "Debug View"), 0)
      .Tooltip(LOC_TEXT("view.tip",
                        "Replace the final image with an internal buffer, for "
                        "inspecting the pipeline."))
      .Items(&DebugViewItems);

  // The hint that the view belongs to the selected pass is not a setting.
  debug.AddHookAfter(debug_log::kDebugView, "DebugViewHint",
                     [](const void*, ctl::ValueStore&, const ctl::DrawOptions&,
                        const std::string&, const ctl::InstanceKey&, bool&) {
                       ImGui::TextDisabled(
                           "%s",
                           LOC_TEXT("view.hint",
                                    "The view is rendered by whichever pass is "
                                    "currently selected in the tabs below, and "
                                    "only takes effect while that pass is "
                                    "enabled.")
                               .c_str());
                       return false;
                     });
}

}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

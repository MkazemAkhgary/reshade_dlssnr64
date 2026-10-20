// Preferences schema, declared inline.

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "feature/preferences/settings.h"

namespace reshade_dlssnr {
namespace feature {

namespace {


}  // namespace

void RegisterPreferencesSchema(ctl::Schema& schema) {
  ctl::Section& prefs = schema.AddSection(preferences::kSection, ctl::SectionKind::Single);

  prefs
      .Add<ctl::BoolSetting>(preferences::kShowTooltips, "Show Tooltips", true)
      .Tooltip("Show contextual descriptions when hovering controls and "
               "section headers.");

  prefs
      .Add<ctl::BoolSetting>(preferences::kCloneResources,
                             "Clone Game Resources", false)
      .Tooltip(
          "Off (default): read the game's current depth/motion resources and "
          "the DLSS output directly. On: copy current-frame depth/motion, and "
          "the DLSS output when Post-DLSS is enabled, into addon-owned "
          "textures. This one option applies to all stages, including "
          "Backbuffer; the requested mode is latched safely at a new DLSS "
          "evaluation.");
}

}  // namespace feature
}  // namespace reshade_dlssnr

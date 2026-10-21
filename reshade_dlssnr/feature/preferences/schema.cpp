// Preferences schema, declared inline.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "feature/preferences/settings.h"

namespace reshade_dlssnr {
namespace feature {

namespace {


}  // namespace

void RegisterPreferencesSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterPreferencesSchema");
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

  prefs
      .Add<ctl::BoolSetting>(preferences::kInvertFallbackDepth,
                             "Invert Fallback Depth", false)
      .Tooltip(
          "Invert the ReShade Generic Depth values when they are used because "
          "the DLSS evaluation did not supply a depth resource. Does not alter "
          "depth supplied by DLSS.");

  prefs
      .Add<ctl::BoolSetting>(preferences::kFp16Storage, "FP16 Pipeline", true)
      .Tooltip(
          "Store the HDR gain-map carrier, model working textures, and "
          "unlifted composition reference in RGBA16F instead of RGBA32F. "
          "Halves the bandwidth and footprint of the HDR pipeline at the "
          "cost of quantization in the carrier-to-HDR multiplier and the "
          "model-domain deltas. SDR paths are unaffected. Toggling resets "
          "temporal history and rebuilds the NGX feature.");
}

}  // namespace feature
}  // namespace reshade_dlssnr

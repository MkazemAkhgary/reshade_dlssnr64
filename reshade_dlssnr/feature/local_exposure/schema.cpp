// Local Exposure schema: one pass-keyed section, no layer stack.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/local_exposure/settings.h"

#define LOC_NAMESPACE "local_exposure"

namespace reshade_dlssnr {
namespace feature {

void RegisterLocalExposureSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterLocalExposureSchema");
  using namespace local_exposure;

  const localization::TextRef kCatAdvanced =
      LOC_TEXT("cat.advanced", "Advanced");

  ctl::Section& local =
      schema.AddSection(kSection, ctl::SectionKind::Instanced);
  local.Title(LOC_TEXT("title", "Local Exposure"));
  local.HeaderToggle(kEnabled);
  local.Add<ctl::BoolSetting>(kEnabled,
                              LOC_TEXT("enabled", "Local Exposure"), false)
      .Tooltip(LOC_TEXT("enabled.tip",
                        "Apply a spatially varying exposure correction, pivoting on a "
                        "display-referred midtone. Works on any source: the HDR path "
                        "reaches the pivot through its tone curve, a display-referred "
                        "path reads it from the image."));
  local.Add<ctl::FloatSetting>(kStrength, LOC_TEXT("strength", "Strength"), 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("strength.tip",
                        "Overall amount of local exposure correction."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kMaxBrighten,
                               LOC_TEXT("max_brighten", "Max Brighten"), 2.50f)
      .ClampRange(0.0f, 8.0f)
      .Tooltip(LOC_TEXT("max_brighten.tip",
                        "Maximum shadow lift in stops. At full bright-detail protection, pixels above "
                        "the midtone pivot are not lifted even when their neighborhood is dark."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kMaxDarken,
                               LOC_TEXT("max_darken", "Max Darken"), 2.50f)
      .ClampRange(0.0f, 8.0f)
      .Tooltip(LOC_TEXT("max_darken.tip",
                        "Maximum highlight reduction in stops. At full dark-detail protection, pixels below "
                        "the pivot are not darkened even when their neighborhood is bright."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kMidtone,
                               LOC_TEXT("midtone", "Local Midtone"), 0.18f)
      .ClampRange(0.01f, 0.5f)
      .Tooltip(LOC_TEXT("midtone.tip",
                        "Target luminance pivot in the model's SDR domain. Pixels near "
                        "this pivot receive little correction; shadows move up and "
                        "highlights move down toward it."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kShadowStrength,
                               LOC_TEXT("shadow_lift", "Shadow Lift"), 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("shadow_lift.tip",
                        "Independent multiplier for shadow lifting. Set to zero to leave "
                        "dark regions where they are while still reducing highlights."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kHighlightStrength,
                               LOC_TEXT("highlight_reduction", "Highlight Reduction"),
                               1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("highlight_reduction.tip",
                        "Independent multiplier for highlight reduction. Set to zero to "
                        "leave bright regions where they are while still lifting shadows."))
      .EnableWhen(kEnabled);
  local.Add<ctl::BoolSetting>(kRestoreEnabled,
                              LOC_TEXT("restore", "Restore Model Exposure"), false)
      .Tooltip(LOC_TEXT("restore.tip",
                        "Apply a correction to the model output after evaluation. When "
                        "disabled, the model's reconstructed local shading is left alone."))
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kRestoreMode,
                               LOC_TEXT("restore_scale", "Restore Spatial Scale"),
                               0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("restore_scale.tip",
                        "Controls the spatial scale of exposure restoration: 0 = exact "
                        "per-pixel cancellation of the local lift, 1 = uniform global "
                        "correction, with log-space blending between them. Global restore "
                        "retains more of the model's reconstructed brightness relations."))
      .EnableWhen(kRestoreEnabled);
  local.Add<ctl::FloatSetting>(kRestoreStrength,
                               LOC_TEXT("restore_strength", "Restore Strength"), 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("restore_strength.tip",
                        "Amount of exposure restoration: 0 = no correction, 1 = full correction. "
                        "Intermediate values blend the selected restoration in log-exposure space."))
      .EnableWhen(kRestoreEnabled);
  local.Add<ctl::FloatSetting>(kRadius,
                               LOC_TEXT("radius", "Local Radius"), 8.0f)
      .ClampRange(1.0f, 32.0f)
      .Tooltip(LOC_TEXT("radius.tip",
                        "Radius in source pixels. A radius of 1 disables multi-tap "
                        "neighborhood sampling and uses only the center pixel. Larger "
                        "radii respond to broader dark/bright regions instead of "
                        "individual pixel noise."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::IntSetting>(kSamples,
                             LOC_TEXT("samples", "Neighborhood Samples"), 8)
      .ClampRange(4, 32)
      .Tooltip(LOC_TEXT("samples.tip",
                        "Requested spatial taps in the local log-luminance estimate. The effective "
                        "count rounds to groups of four to keep sampling spatially balanced."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kShadowRange,
                               LOC_TEXT("shadow_transition", "Shadow Transition"),
                               1.0f)
      .ClampRange(0.25f, 6.0f)
      .Tooltip(LOC_TEXT("shadow_transition.tip",
                        "Stops from the midtone pivot over which shadow lift ramps to full "
                        "strength. A smaller value acts more aggressively on dark regions."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kHighlightRange,
                               LOC_TEXT("highlight_transition",
                                        "Highlight Transition"),
                               1.0f)
      .ClampRange(0.25f, 6.0f)
      .Tooltip(LOC_TEXT("highlight_transition.tip",
                        "Stops above the midtone pivot over which highlight reduction "
                        "ramps to full strength. A smaller value acts more aggressively."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kHighlightProtection,
                               LOC_TEXT("bright_detail_protection",
                                        "Bright Detail Protection"),
                               1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("bright_detail_protection.tip",
                        "Controls whether a dark neighborhood may lift pixels above the pivot. "
                        "1 keeps bright spots anchored; 0 allows correction across the pivot."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kShadowProtection,
                               LOC_TEXT("dark_detail_protection",
                                        "Dark Detail Protection"),
                               1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("dark_detail_protection.tip",
                        "Controls whether a bright neighborhood may darken pixels below the pivot. "
                        "1 keeps dark details anchored; 0 allows correction across the pivot."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kDetailRetention,
                               LOC_TEXT("detail_retention", "Detail Retention"),
                               1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("detail_retention.tip",
                        "Retains the pixel's log-luminance detail relative to its local "
                        "base. 1 preserves all relative detail; lower values compress it."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
  local.Add<ctl::FloatSetting>(kEdgeProtection,
                               LOC_TEXT("edge_protection", "Edge Protection"), 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("edge_protection.tip",
                        "Reduces the influence of samples across strong log-luminance edges. "
                        "0 uses spatial weights only; higher values reduce halos but can "
                        "make adaptation more local."))
      .Category(kCatAdvanced)
      .EnableWhen(kEnabled);
}

}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

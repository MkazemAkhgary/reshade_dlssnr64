// The exposure-filter rows. See rows.h.
//
// Only Adaptive Exposure declares these; the Game layer is a scalar bias. The
// parameter rows
// are gated by the checkbox that enables them. The gate reads a checkbox in
// the section it is drawn in; the framework hands the predicate that section
// name, so the only thing this file has to remember is which checkbox -- and
// that is the predicate's context, a stable string literal.

#include "rows.h"
#include "profiling/profile.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "ctl/values.h"

#define LOC_NAMESPACE "hdr_filters"

namespace reshade_dlssnr {
namespace hdr_filter_rows {

namespace {

using namespace names;

// The context is the name of the checkbox this row follows.
bool Checked(const void* context, const ctl::ValueStore& store,
             const ctl::InstanceKey& key, const std::string& section) {
  DLSSNR_PROFILE_CTL_L3("Checked");
  const char* checkbox = static_cast<const char*>(context);
  return store.Read(section, key, checkbox).as_bool;
}

}  // namespace

void Declare(ctl::Section& section, const localization::TextRef& category) {
  DLSSNR_PROFILE_CTL_L3("Declare");
  section.Add<ctl::BoolSetting>(kExposureMask,
                                LOC_TEXT("exposure_mask", "Exposure Mask"), false)
      .Tooltip(LOC_TEXT("exposure_mask.tip",
                        "Meter or correct only pixels weighted by the persistent "
                        "Exposure Mask mask."))
      .Category(category);
  section.Add<ctl::BoolSetting>(kDepth, LOC_TEXT("depth", "Depth"), false)
      .Tooltip(LOC_TEXT("depth.tip",
                        "Restrict this layer to the selected normalized depth interval."))
      .Category(category);
  section.Add<ctl::BoolSetting>(kPercentileRange,
                                LOC_TEXT("percentile_range", "Percentile Range"),
                                false)
      .Tooltip(LOC_TEXT("percentile_range.tip",
                        "For Game/Adaptive, select a percentile band of this layer's "
                        "histogram. For Local Exposure, fade the local correction "
                        "outside its measured luminance band."))
      .Category(category);

  section.Add<ctl::FloatSetting>(kDepthNear,
                                 LOC_TEXT("depth_near", "Depth Near"), 0.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("depth_near.tip",
                        "Near end of the accepted normalized depth interval."))
      .EnableWhen(&Checked, kDepth)
      .Category(category);
  section.Add<ctl::FloatSetting>(kDepthFar, LOC_TEXT("depth_far", "Depth Far"),
                                 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("depth_far.tip",
                        "Far end of the accepted normalized depth interval."))
      .EnableWhen(&Checked, kDepth)
      .Category(category);

  section.Add<ctl::FloatSetting>(kPercentileLow,
                                 LOC_TEXT("percentile_low", "Low Percentile"),
                                 0.70f)
      .ClampRange(0.0f, 0.99f)
      .Tooltip(LOC_TEXT("percentile_low.tip",
                        "Lower percentile boundary. Game/Adaptive use the band for "
                        "metering; Local uses it as a luminance protection band."))
      .EnableWhen(&Checked, kPercentileRange)
      .Category(category);
  section.Add<ctl::FloatSetting>(kPercentileHigh,
                                 LOC_TEXT("percentile_high", "High Percentile"),
                                 0.90f)
      .ClampRange(0.01f, 1.0f)
      .Tooltip(LOC_TEXT("percentile_high.tip",
                        "Upper percentile boundary for this layer."))
      .EnableWhen(&Checked, kPercentileRange)
      .Category(category);
}

}  // namespace hdr_filter_rows
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

// The exposure-filter rows. See rows.h.
//
// Only Adaptive Exposure declares these; the Game layer is a scalar bias. The
// parameter rows
// are gated by the checkbox that enables them. The gate reads a checkbox in
// the section it is drawn in; the framework hands the predicate that section
// name, so the only thing this file has to remember is which checkbox -- and
// that is the predicate's context, a stable string literal.

#include "rows.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "ctl/values.h"

namespace reshade_dlssnr {
namespace hdr_filter_rows {

namespace {

using namespace names;

// The context is the name of the checkbox this row follows.
bool Checked(const void* context, const ctl::ValueStore& store,
             const ctl::InstanceKey& key, const std::string& section) {
  const char* checkbox = static_cast<const char*>(context);
  return store.Read(section, key, checkbox).as_bool;
}

}  // namespace

void Declare(ctl::Section& section, const char* category) {
  section.Add<ctl::BoolSetting>(kImportanceMask, "Importance Mask", false)
      .Tooltip("Meter or correct only pixels weighted by the persistent "
               "Importance Mask mask.")
      .Category(category);
  section.Add<ctl::BoolSetting>(kDepth, "Depth", false)
      .Tooltip("Restrict this layer to the selected normalized depth interval.")
      .Category(category);
  section.Add<ctl::BoolSetting>(kPercentileRange, "Percentile Range", false)
      .Tooltip("For Game/Adaptive, select a percentile band of this layer's "
               "histogram. For Local Exposure, fade the local correction "
               "outside its measured luminance band.")
      .Category(category);

  section.Add<ctl::FloatSetting>(kDepthNear, "Depth Near", 0.0f)
      .Range(0.0f, 1.0f)
      .Tooltip("Near end of the accepted normalized depth interval.")
      .EnableWhen(&Checked, kDepth)
      .Category(category);
  section.Add<ctl::FloatSetting>(kDepthFar, "Depth Far", 1.0f)
      .Range(0.0f, 1.0f)
      .Tooltip("Far end of the accepted normalized depth interval.")
      .EnableWhen(&Checked, kDepth)
      .Category(category);

  section.Add<ctl::FloatSetting>(kPercentileLow, "Low Percentile", 0.70f)
      .Range(0.0f, 0.99f)
      .Tooltip("Lower percentile boundary. Game/Adaptive use the band for "
               "metering; Local uses it as a luminance protection band.")
      .EnableWhen(&Checked, kPercentileRange)
      .Category(category);
  section.Add<ctl::FloatSetting>(kPercentileHigh, "High Percentile", 0.90f)
      .Range(0.01f, 1.0f)
      .Tooltip("Upper percentile boundary for this layer.")
      .EnableWhen(&Checked, kPercentileRange)
      .Category(category);
}

}  // namespace hdr_filter_rows
}  // namespace reshade_dlssnr

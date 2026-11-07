// Exposure Mask schema.
//
// The mask is built by the first Pre/Post-DLSS pass from the model's actual
// input/output difference. Every row is gated by the section toggle alone.
//
// The section as a whole is gated by conditions that belong to other features
// and to the pass. They are declared here because a section gate is a property
// of the section; the names are constants, so a rename is a compile error.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/exposure_mask/settings.h"
#include "feature/hdr/settings.h"
#include "feature/pass/settings.h"
#include "fragments/hdr_filter_rows/rows.h"
#include "state/settings.h"

#define LOC_NAMESPACE "exposure_mask"

namespace reshade_dlssnr {
namespace feature {
namespace exposure_mask {

namespace {

// The mask is metering input for Adaptive Exposure: without HDR metering, that
// layer, or that layer's mask checkbox, there is nothing to feed. A Backbuffer
// pass has no scene-referred source to measure the mask against.
bool SectionEnabled(const void*, const ctl::ValueStore& store,
                    const ctl::InstanceKey& key, const std::string&) {
  const int target = store.Read(pass::kSection, key, pass::kTarget).as_int;
  if (target == static_cast<int>(InjectionStage::Backbuffer)) return false;
  return store.Read(hdr::kSection, key, hdr::kEnable).as_bool &&
         store.Read(hdr::kAdaptiveSection, key, hdr::kAdaptiveEnabled).as_bool &&
         store.Read(hdr::kAdaptiveSection, key,
                    hdr_filter_rows::names::kExposureMask).as_bool;
}

}  // namespace

}  // namespace exposure_mask

void RegisterExposureMaskSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterExposureMaskSchema");
  using namespace exposure_mask;

  ctl::Section& mask =
      schema.AddSection(kSection, ctl::SectionKind::Instanced);
  mask.Title(LOC_TEXT("title", "Exposure Mask"));
  mask.HeaderToggle(kEnable);
  mask.EnableSectionWhen(&SectionEnabled);

  mask.Add<ctl::BoolSetting>(kEnable, LOC_TEXT("enabled", "Enable"), false)
      .Tooltip(LOC_TEXT("enabled.tip",
                        "This pass builds its Exposure Mask after model evaluation. "
                        "The mask affects only this pass's next-frame HDR metering."));

  mask.Add<ctl::FloatSetting>(kEditStrength,
                              LOC_TEXT("edit_strength", "Edit Strength"), 0.5f)
      .ClampRange(0.1f, 8.0f)
      .Tooltip(LOC_TEXT("edit_strength.tip",
                        "Scale the measured model-input/output difference before "
                        "thresholding."))
      .EnableWhen(kEnable);
  mask.Add<ctl::FloatSetting>(kSmoothing,
                              LOC_TEXT("smoothing", "Spatial Smoothing"), 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("smoothing.tip",
                        "Majority-filter the binary exposure votes in a small "
                        "neighborhood."))
      .EnableWhen(kEnable);
  mask.Add<ctl::FloatSetting>(kResponse,
                              LOC_TEXT("response", "Temporal Response"), 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("response.tip",
                        "Blend new mask votes with the previous frame's mask."))
      .EnableWhen(kEnable);
  mask.Add<ctl::FloatSetting>(kLumaThreshold,
                              LOC_TEXT("luma_threshold", "Luma Threshold"), 0.02f)
      .ClampRange(0.0f, 0.5f)
      .Tooltip(LOC_TEXT("luma_threshold.tip",
                        "Minimum absolute luma difference to mark a model edit."))
      .EnableWhen(kEnable);
  mask.Add<ctl::FloatSetting>(kLumaRelative,
                              LOC_TEXT("luma_relative", "Relative Luma"), 0.05f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("luma_relative.tip",
                        "Minimum luma difference relative to the brighter input/output "
                        "luma."))
      .EnableWhen(kEnable);
  mask.Add<ctl::FloatSetting>(kChromaThreshold,
                              LOC_TEXT("chroma_threshold", "Chroma Threshold"),
                              0.02f)
      .ClampRange(0.0f, 0.5f)
      .Tooltip(LOC_TEXT("chroma_threshold.tip",
                        "Minimum chroma-only difference to mark a model edit."))
      .EnableWhen(kEnable);
}

}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

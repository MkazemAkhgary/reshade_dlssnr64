// Importance Mask schema.
//
// The mask is built by the first Pre/Post-DLSS pass from the model's actual
// input/output difference. Every row is gated by the section toggle alone.
//
// The other condition -- "HDR is on" -- is not expressed here. It is a fact
// about a different feature, and this file has no way to read it except by
// naming HDR's keys, which would silently break if HDR renamed one. The
// overlay hosts both sections, so it owns that coupling: it disables this
// whole section when HDR is off. See ui/ui.cpp.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/importance_mask/settings.h"

namespace reshade_dlssnr {
namespace feature {
namespace importance_mask {

namespace {

// Interactive only while the mask is on.
bool MaskEditable(const void*, const ctl::ValueStore& store,
                  const ctl::InstanceKey& key, const std::string& section) {
  DLSSNR_PROFILE_CTL_L3("MaskEditable");
  return store.Read(section, key, kEnable).as_bool;
}

}  // namespace

}  // namespace importance_mask

void RegisterImportanceMaskSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterImportanceMaskSchema");
  using namespace importance_mask;

  ctl::Section& mask =
      schema.AddSection(kSection, ctl::SectionKind::Instanced);
  mask.HeaderToggle(kEnable);

  mask.Add<ctl::BoolSetting>(kEnable, "Enable", false)
      .Tooltip("This pass builds its Importance Mask after model evaluation. "
               "The mask affects only this pass's next-frame HDR metering.");

  mask.Add<ctl::FloatSetting>(kEditStrength, "Edit Strength", 1.0f)
      .ClampRange(0.1f, 8.0f)
      .Tooltip("Scale the measured model-input/output difference before "
               "thresholding.")
      .EnableWhen(&MaskEditable);
  mask.Add<ctl::FloatSetting>(kSmoothing, "Spatial Smoothing", 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Majority-filter the binary importance votes in a small "
               "neighborhood.")
      .EnableWhen(&MaskEditable);
  mask.Add<ctl::FloatSetting>(kResponse, "Temporal Response", 0.15f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Blend new mask votes with the previous frame's mask.")
      .EnableWhen(&MaskEditable);
  mask.Add<ctl::FloatSetting>(kLumaThreshold, "Luma Threshold", 0.02f)
      .ClampRange(0.0f, 0.5f)
      .Tooltip("Minimum absolute luma difference to mark a model edit.")
      .EnableWhen(&MaskEditable);
  mask.Add<ctl::FloatSetting>(kLumaRelative, "Relative Luma", 0.05f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Minimum luma difference relative to the brighter input/output "
               "luma.")
      .EnableWhen(&MaskEditable);
  mask.Add<ctl::FloatSetting>(kChromaThreshold, "Chroma Threshold", 0.02f)
      .ClampRange(0.0f, 0.5f)
      .Tooltip("Minimum chroma-only difference to mark a model edit.")
      .EnableWhen(&MaskEditable);
}

}  // namespace feature
}  // namespace reshade_dlssnr

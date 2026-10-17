#include "feature/ui_state/settings.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"

namespace reshade_dlssnr {
namespace feature {

void RegisterUiStateSchema(ctl::Schema& schema) {
  ctl::Section& section =
      schema.AddSection(ui_state::kSection, ctl::SectionKind::Single);
  section.Add<ctl::BoolSetting>(ui_state::kStatusOpen, "Status Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kPreferencesOpen,
                                "Preferences Open", false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kPresetsOpen, "Presets Open", false)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kPassesOpen, "Passes Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kDebugOpen, "Debug Open", false)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kHdrOpen, "HDR Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kGradeOpen, "Grade Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kHdrDiagnosticsOpen,
                                "HDR Diagnostics Open", false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kImportanceMaskOpen,
                                "Importance Mask Open", true).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kHdrGameOpen,
                                "HDR Game Exposure Open", true).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kHdrAdaptiveOpen,
                                "HDR Adaptive Exposure Open", false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kHdrLocalOpen,
                                "HDR Local Exposure Open", false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kModelOpen, "Model Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kStrengthOpen, "Strength Open", true)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kTemporalOpen,
                                "Temporal Reprojection Open", true).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kSoftCompressionOpen,
                                "Soft Compression Open", true).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kResidualOpen, "Residual Edits Open",
                                false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kRcasOpen, "RCAS Open", false)
      .GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kGradeInputOpen,
                                "Grade Input Open", false).GlobalOnly();
  section.Add<ctl::BoolSetting>(ui_state::kGradeOutputOpen,
                                "Grade Output Open", false).GlobalOnly();
}

}  // namespace feature
}  // namespace reshade_dlssnr

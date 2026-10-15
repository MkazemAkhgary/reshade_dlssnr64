// Overlay UI-state setting names. Persisted globally, never inside profiles.
#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace ui_state {

inline constexpr const char* kSection = "Overlay UI State";
inline constexpr const char* kStatusOpen = "status_open";
inline constexpr const char* kPreferencesOpen = "preferences_open";
inline constexpr const char* kPresetsOpen = "presets_open";
inline constexpr const char* kPassesOpen = "passes_open";
inline constexpr const char* kDebugOpen = "debug_open";
inline constexpr const char* kHdrOpen = "hdr_open";
inline constexpr const char* kGradeOpen = "grade_open";
inline constexpr const char* kHdrDiagnosticsOpen = "hdr_diagnostics_open";
inline constexpr const char* kBoundedTuningOpen = "bounded_tuning_open";
inline constexpr const char* kImportanceMaskOpen = "importance_mask_open";
inline constexpr const char* kModelOpen = "model_open";
inline constexpr const char* kStrengthOpen = "strength_open";
inline constexpr const char* kTemporalOpen = "temporal_open";
inline constexpr const char* kSoftCompressionOpen = "soft_compression_open";
inline constexpr const char* kAdvancedOpen = "advanced_open";
inline constexpr const char* kResidualOpen = "residual_open";
inline constexpr const char* kRcasOpen = "rcas_open";
inline constexpr const char* kGradeInputOpen = "grade_input_open";
inline constexpr const char* kGradeOutputOpen = "grade_output_open";

}  // namespace ui_state
}  // namespace feature
}  // namespace reshade_dlssnr

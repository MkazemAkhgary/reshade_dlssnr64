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
inline constexpr const char* kImportanceMaskOpen = "importance_mask_open";
inline constexpr const char* kHdrAdvancedOpen = "hdr_advanced_open";
inline constexpr const char* kHdrGameOpen = "hdr_game_open";
inline constexpr const char* kHdrAdaptiveFiltersOpen = "hdr_adaptive_filters_open";
inline constexpr const char* kHdrAdaptiveOpen = "hdr_adaptive_open";
inline constexpr const char* kHdrLocalOpen = "hdr_local_open";
inline constexpr const char* kHdrLocalAdvancedOpen = "hdr_local_advanced_open";
inline constexpr const char* kModelOpen = "model_open";
inline constexpr const char* kStrengthOpen = "strength_open";
inline constexpr const char* kTemporalOpen = "temporal_open";
inline constexpr const char* kResidualOpen = "residual_open";
inline constexpr const char* kGradeInputOpen = "grade_input_open";
inline constexpr const char* kGradeOutputOpen = "grade_output_open";

}  // namespace ui_state
}  // namespace feature
}  // namespace reshade_dlssnr

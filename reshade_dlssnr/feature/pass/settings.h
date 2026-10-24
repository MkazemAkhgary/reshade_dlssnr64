// Pass setting names. A leaf header: names only.
//
// The pass section is instanced -- one instance per pass -- so no name here
// carries a pass index. Identity comes from the instance key, never the name.

#pragma once

namespace reshade_dlssnr {
namespace feature {
namespace pass {

// Section names. The pass feature owns three: the pass itself, and the two
// model-boundary grades it hosts.
inline constexpr const char* kOrderSection = "Pass Order";
inline constexpr const char* kOrderSetting = "ids";
inline constexpr const char* kSection = "Pass";
inline constexpr const char* kGradeInputSection = "Grade Model Input";
inline constexpr const char* kGradeOutputSection = "Grade Model Output";

// The name prefix each hosted grade's rows were declared under. A reader that
// looks a grade row up by name (feature/grade/view.cpp) needs the prefix, so
// it lives beside the section it applies to rather than in the reader.
inline constexpr const char* kGradeInputPrefix = "grade_input.";
inline constexpr const char* kGradeOutputPrefix = "grade_output.";

// Section "Pass".
inline constexpr const char* kPassEnabled = "pass_enabled";
inline constexpr const char* kTarget = "target";
// Free-text tab label. Empty means show the positional default ("Pass N").
inline constexpr const char* kCustomName = "custom_name";
inline constexpr const char* kWorkingScale = "working_scale";
inline constexpr const char* kWorkingScaleX2 = "working_scale_x2";
inline constexpr const char* kPreset = "preset";
inline constexpr const char* kIntensity = "intensity";
inline constexpr const char* kLocalTone = "local_tone";
inline constexpr const char* kLocalStructure = "local_structure";
inline constexpr const char* kAutoMask = "auto_mask";
inline constexpr const char* kSkinStructure = "skin_structure";
inline constexpr const char* kUiCorrection = "ui_correction";
inline constexpr const char* kLumaStrength = "luma_strength";
inline constexpr const char* kChromaStrength = "chroma_strength";
inline constexpr const char* kTemporalMode = "temporal_mode";
inline constexpr const char* kTemporalCadence = "temporal_cadence";
inline constexpr const char* kSyntheticMotionGuides = "synthetic_motion_guides";
inline constexpr const char* kTemporalPhotometricAbsThreshold = "temporal_photometric_abs_threshold";
inline constexpr const char* kTemporalPhotometricRelThreshold = "temporal_photometric_rel_threshold";
inline constexpr const char* kTemporalDepthAbsThreshold = "temporal_depth_abs_threshold";
inline constexpr const char* kTemporalDepthRelThreshold = "temporal_depth_rel_threshold";
inline constexpr const char* kTemporalMaxDisplacement = "temporal_max_displacement";
inline constexpr const char* kTemporalMaxInvalidPercent = "temporal_max_invalid_percent";
inline constexpr const char* kTemporalFp16History = "temporal_fp16_history";
inline constexpr const char* kTemporalCheapScale = "temporal_cheap_scale";

// Category "Advanced".
inline constexpr const char* kEditStrength = "edit_strength";
inline constexpr const char* kResidualChroma = "residual_chroma";
inline constexpr const char* kResidualGamma = "residual_gamma";
inline constexpr const char* kResidualHighlight = "residual_highlight";
inline constexpr const char* kResidualShadow = "residual_shadow";
inline constexpr const char* kUpsampleSharpness = "upsample_sharpness";
inline constexpr const char* kRatioEps = "ratio_eps";
inline constexpr const char* kRatioBrighten = "ratio_brighten";
inline constexpr const char* kRatioDarken = "ratio_darken";
inline constexpr const char* kRatioClampsEnabled = "ratio_clamps_enabled";

// The two model grade sections. Each is a section of its own, so "enabled" is
// unqualified within it.
inline constexpr const char* kGradeEnabled = "enabled";

// Input-grade behavior: when set, the model edit is measured against the graded
// input and applied to the pre-grade image, so the input grade changes what the
// model sees without being baked into the final result.
inline constexpr const char* kGradeInputApplyToPreGrade = "apply_to_pre_grade";

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr

// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

namespace reshade_dlssnr {

constexpr uint32_t kMaxPasses = 3;

// Storage capacity for PassSettings::params and ::inherit_params. Kept
// larger than the current table on purpose; settings.cpp static_asserts
// the two stay in step.
constexpr uint32_t kShaderParamCapacity = 32;

enum class InjectionStage : int {
  PostDLSS = 0,
  Present = 1,
};

// How the source image is encoded. Auto means detect from DXGI format.
enum class HdrFormat : int {
  Auto  = 0,
  Sdr   = 1,   // sRGB-encoded, [0,1]
  ScRgb = 2,   // linear, 1.0 == 80 nits
  Hdr10 = 3,   // PQ (ST.2084)
};

// The code the shaders read, which is not the enum's own value: 0 is sRGB,
// 1 is scRGB and 2 is HDR10. Auto is a request, not an encoding, so it
// resolves to SDR here — by the time a shader sees a format the resource has
// already been detected and the enum is no longer Auto.
inline int HdrFormatCode(HdrFormat format) {
  switch (format) {
    case HdrFormat::ScRgb: return 1;
    case HdrFormat::Hdr10: return 2;
    case HdrFormat::Auto:
    case HdrFormat::Sdr:
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// Residual edit
// ---------------------------------------------------------------------------
//
// How the model's edit is carried onto the source. Always a per-channel
// ratio (scale-invariant, so one setting serves SDR and HDR); an additive
// transfer was dropped for artifacts on high-contrast edges. Per pass.
struct ResidualSettings {
  // Scales the edit before it is applied. 0 makes the compose a pass-through
  // (only the model's own evaluation still runs), 1 applies it at the
  // magnitude the model produced, above 1 amplifies it.
  float edit_strength = 1.0f;

  // How much of the edit's chroma lands relative to its luma. 1 keeps the
  // model's per-channel delta intact; 0 leaves only the luma part and takes
  // hue out of the edit entirely, which is the setting for a scene where the
  // model's chroma shift reads as wrong.
  float chroma_strength = 1.0f;

  // Response curve on the edit. 1 is the model's own curve; below 1 amplifies
  // small edits, above 1 compresses them. Shape only: 0 magnitude stays 0.
  float edit_gamma = 1.0f;

  // Region weighting. Both reduce the edit where they apply, so the residual
  // stays visible in the midtones and stops fighting whatever the model did
  // at the ends of the range.
  //   highlight_protect: 1 removes the edit entirely above SDR white.
  //   shadow_protect: 1 removes the edit entirely below SDR black.
  float highlight_protect = 0.0f;
  float shadow_protect = 0.0f;
};

// ---------------------------------------------------------------------------
// Colour grade
// ---------------------------------------------------------------------------
//
// A grade the user applies at three points: before the model reads, after
// it writes, and on the final backbuffer. One luma mode and one chroma mode
// are active at a time; stacking two would make them fight.

// How the grade changes brightness.
enum class GradeLumaMode : int {
  Gain = 0,   // x2^amount; symmetric, clips at white
  Gamma = 1,  // power on normalized luma; midtone-weighted, never clips
  Lift = 2,   // offset scaled by headroom; moves shadows
  Soft = 3,   // parabolic lift; strongest at mid grey
};

// How the grade changes saturation.
enum class GradeChromaMode : int {
  Saturation = 0,  // blend toward luma; uniform
  Vibrance = 1,    // muted colors move more
  ChromaGain = 2,  // scale chroma only; luma fixed
};

// One grade. Where it is applied is decided by which field holds it, not by
// anything in here.
//
// The grade runs in linear light, so every value below is a linear-light
// quantity rather than a display-encoded one. That is what keeps a bright
// highlight's shape instead of flattening it: the operators act on the light
// the pixels actually carry.
struct GradeSettings {
  bool enabled = false;

  // Both in [-1, 1], 0 = unchanged. The ranges are symmetric so the midpoint
  // of the slider is the neutral position.
  float luma = 0.0f;
  float chroma = 0.0f;

  GradeLumaMode luma_mode = GradeLumaMode::Gamma;
  GradeChromaMode chroma_mode = GradeChromaMode::Saturation;

  // Levels on linear luma, applied first. Black point is the value mapped
  // to black (raising crushes, lowering lifts). White point is the value
  // mapped to white.
  float black_point = 0.0f;   // [kGradeBlackPointMin, kGradeBlackPointMax]
  float white_point = 1.0f;   // [kGradeWhitePointMin, kGradeWhitePointMax]

  // Contrast, a stretch around mid grey. Positive steepens, negative flattens.
  float contrast = 0.0f;      // [-1, 1]

  // Region-weighted scales; each fades out before reaching the other end.
  float shadows = 0.0f;       // [-1, 1]
  float highlights = 0.0f;    // [-1, 1]
};

// The two level ranges are named because the loader clamps to them and the UI
// draws sliders over them: a slider wider than the clamp is a control that
// springs back when the config is reloaded.
constexpr float kGradeBlackPointMin = -0.25f;
constexpr float kGradeBlackPointMax = 0.25f;
constexpr float kGradeWhitePointMin = 0.2f;
constexpr float kGradeWhitePointMax = 4.0f;

struct PassSettings {
  int preset = 1;

  float local_tone = 1.0f;
  bool ui_correction = true;

  float local_structure = 1.0f;
  bool auto_mask = true;
  float skin_structure = 1.0f;

  float intensity = 1.0f;

  float working_scale = 1.0f;

  // Where this pass runs. Constraint: if any earlier pass runs at Present,
  // this pass must also run at Present. Enforced at load time and in the UI.
  InjectionStage injection_stage = InjectionStage::Present;

  // Grades at this pass's two model boundaries. Not inheritable — carrying
  // one into the next pass would apply it twice.
  GradeSettings grade_input;
  GradeSettings grade_output;

  // How this pass's model edit is carried back. Not inheritable.
  ResidualSettings residual;

  // FSR RCAS on the EASU-upsampled working images. Not inheritable.
  bool rcas_enabled = false;
  float rcas_strength = 0.5f;  // [0, 1]

  // Only meaningful for pass index > 0; a true flag makes the slider use
  // the previous pass's resolved value.
  bool inherit_working_scale = false;
  bool inherit_intensity = false;
  bool inherit_local_tone = false;
  bool inherit_local_structure = false;
  bool inherit_skin_structure = false;
  bool inherit_params[kShaderParamCapacity] = {};

  // Dynamic shader parameter values, indexed by kShaderParams order.
  float params[kShaderParamCapacity] = {};

  // Display name for the pass tab. Purely cosmetic; empty means use the
  // positional default ("Pass N").
  char custom_name[64] = {};

};

struct Settings {
  bool enabled = true;
  bool only_super_sampling = true;

  // When true, hover tooltips are drawn in the overlay. Exposed next to
  // the master switch as "Show Tooltips"; defaults to on.
  bool show_tooltips = true;

  // Debug view selection, shared across all passes. 0 = off; N (1-based)
  // selects kDebugViews[N-1] in runtime/debug_view.h. The active pass
  // renders the selected view; switching tabs keeps the selection.
  int debug_view = 0;

  bool pass_enabled[kMaxPasses] = {true, false, false};
  PassSettings passes[kMaxPasses];

  // UI state: which pass tab is currently open. Determines whose debug view
  // is rendered. Transient (not persisted).
  int active_pass = 0;

  // ---------------------------------------------------------------------
  // HDR handling. Shared across passes; only the Post-DLSS path uses it.
  // See HDR_DESIGN.md for the pipeline and the meaning of each control.
  //
  // How the model's edit is carried back is NOT here: that is
  // PassSettings::residual, per pass, and it applies whether or not HDR
  // handling is on.
  // ---------------------------------------------------------------------
  bool hdr_enable = true;
  HdrFormat hdr_format = HdrFormat::Auto;

  // How often the scene's brightness is measured, in frames. 1 measures
  // every frame (most responsive, most GPU work); higher values measure
  // less often. The EMA blend rate scales with this so a longer interval
  // also smooths more slowly.
  int hdr_measure_interval = 4;

  // Scene key target in SDR (midtones land here).
  float hdr_midtone = 0.18f;

  // Which percentile of the histogram defines the white point.
  float hdr_white_percentile = 0.99f;

  // Highlight headroom: multiplier on the auto-detected white point. 1.0
  // uses the histogram's own value (P99 by default), which clips the top
  // 1% of pixels and flattens highlights on the round trip. The default
  // 8.0 gives generous headroom so highlights survive the round trip
  // intact, at the cost of a slightly flatter overall response.
  float hdr_highlight_headroom = 8.0f;

  // Maximum log2 change to the exposure per measurement. A bright object
  // entering or leaving the frame moves the target by a large amount; the
  // cap converts that into a slow drift. 0 disables the cap. Warmup is
  // exempt regardless, so the first ~30 measurements converge fast.
  float hdr_rate_cap = 0.1f;

  // Skin pixels take priority for the exposure median once they are at
  // least this fraction of the frame. A face is usually a small part of
  // the image even when it is the subject, so the threshold is low.
  float hdr_skin_fraction_threshold = 0.01f;

  // If the depth filter leaves less than this fraction of the frame, the
  // exposure median falls back to the unfiltered histogram rather than
  // metering on too few pixels. Applies only when depth is available.
  float hdr_depth_fallback_fraction = 0.20f;

  // Skin detection: run the model at reduced scale with tone/structure edits
  // suppressed and skin edits on, threshold the difference against its input.
  // The mask replaces the histogram's hue-based classifier. DLSS Output only.
  bool skin_detect_enable = false;
  float skin_detect_scale = 0.25f;
  // Absolute floor for a luma change to count. Below this it is treated as
  // noise regardless of relative size.
  float skin_detect_luma_threshold = 0.02f;
  // Relative bar: a change must also exceed this fraction of the pixel's
  // luma. Rejects the model's numeric jitter on bright pixels.
  float skin_detect_luma_relative = 0.05f;
  float skin_detect_chroma_threshold = 0.02f;
  // Saturation bound: any channel above this on either side excludes the
  // pixel. Rejects emissives and clipped highlights.
  float skin_detect_clip_threshold = 0.97f;

  // Model tuning for the detection pass. Fully independent from the pass
  // sliders: none of these inherit from or share with any PassSettings.
  int skin_detect_preset = 0;
  float skin_detect_intensity = 1.0f;
  float skin_detect_local_tone = 0.0f;
  float skin_detect_local_structure = 0.0f;
  float skin_detect_skin_structure = 1.0f;
  bool skin_detect_ui_correction = false;

  // Diff amplification (raises detection sensitivity without changing what
  // the model produces) and mask smoothing (majority-vote radius, 0..1).
  float skin_detect_edit_strength = 1.0f;
  float skin_detect_mask_smoothing = 0.5f;

  // HDR response for the detection pass's own exposure tracker. Independent
  // of the regular-pass HDR settings. The depth fallback defaults to 1.0 so
  // the tracker always uses the full-frame histogram and never flips source
  // on camera motion.
  float skin_detect_hdr_midtone = 0.18f;
  float skin_detect_hdr_rate_cap = 0.1f;
  int skin_detect_hdr_measure_interval = 16;
  float skin_detect_hdr_depth_fallback_fraction = 1.0f;

  // ---------------------------------------------------------------------
  // Final backbuffer grade. Runs once per frame, after every pass, on the
  // swapchain backbuffer itself — the last thing before the game presents.
  // Global rather than per-pass because there is one backbuffer.
  // ---------------------------------------------------------------------
  GradeSettings grade_backbuffer;

  // UI state: which collapsible sections are open. Persisted, and shared
  // across passes — collapsing a section in one tab collapses it in every
  // tab, so the panel keeps a single, consistent shape. `ui_category_open`
  // is indexed by the order categories appear in kShaderParams (0, 1, ...);
  // entries past the current category count are unused. Model and each
  // category default to open.
  bool ui_model_open = true;
  bool ui_category_open[16] = {
      true, true, true, true, true, true, true, true,
      true, true, true, true, true, true, true, true};
  bool ui_residual_open = true;
  bool ui_rcas_open = true;
  bool ui_status_open = true;
  bool ui_hdr_open = true;
  bool ui_grade_open = true;
  bool ui_hdr_diagnostics_open = false;
  bool ui_skin_detect_open = true;
  bool ui_skin_detect_model_open = true;
  bool ui_skin_detect_mask_open = true;
  bool ui_skin_detect_hdr_open = true;
};

Settings& GetSettings();

// Resolved pass settings with inheritance applied. This is derived state,
// kept in sync by LoadSettings and SaveSettings. The runtime reads it
// directly, so no per-frame resolution is ever needed. Index 0 is always
// the raw settings (there is nothing to inherit from).
//
// Inheritance is a UI / settings-change concern: the resolved array is
// recomputed whenever the settings layer changes, and everyone else just
// reads the result.
const PassSettings& GetResolvedPass(uint32_t index);

void LoadSettings();
void SaveSettings();

}  // namespace reshade_dlssnr

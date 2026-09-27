// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

namespace reshade_dlssnr {

constexpr uint32_t kMaxPasses = 3;

// Storage capacity for PassSettings::params and ::inherit_params.
//
// Deliberately larger than kShaderParamCount, and deliberately not derived
// from it. Sizing the storage from the table would resize PassSettings every
// time a parameter is added, and an array that is merely big enough today
// truncates silently once the table outgrows it: the index is not bounds
// checked, so the write lands on whatever field follows. The static_assert
// in settings.cpp is what keeps the two in step.
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
// How the model's edit is carried onto the source image. The model produces an
// edit at working scale; the compose step applies it to the full-resolution
// original, and this is what governs that application.
//
// The transfer is always a per-channel ratio. An additive transfer was tried
// and dropped: an RGB delta measured in SDR does not survive the trip back to
// a wider range, and it produced visible artifacts around high-contrast edges.
// A ratio is scale-invariant, so the same edit means the same thing whatever
// the original's range is — which is also why one setting can serve both the
// SDR and the HDR compose.
//
// Per pass, because the model's edit is a statement about what that pass
// produced.
struct ResidualSettings {
  // Scales the edit before it is applied. 0 makes the compose a pass-through
  // (only the model's own evaluation still runs), 1 applies it at the
  // magnitude the model produced, above 1 amplifies it.
  float edit_strength = 1.0f;
};

// ---------------------------------------------------------------------------
// Colour grade
// ---------------------------------------------------------------------------
//
// A brightness/saturation grade the user applies to the image at chosen points
// in the frame: before the model reads it, after the model writes it, and on
// the final backbuffer. It is separate from the model's own edit — the model
// still does its thing, this is a grade on top.
//
// Only ONE luma algorithm and ONE chroma algorithm are active at a time, and
// the combos below select which. That is deliberate: several of these do the
// same job by different means, and stacking two of them makes each control
// fight the other, because the second one re-derives the value the first just
// changed. The combo is the switch between them.

// How the grade changes brightness.
enum class GradeLumaMode : int {
  // Multiply luma by 2^amount. The classic exposure control: symmetric
  // (equal push either way), and it clips at white rather than rolling off.
  Gain = 0,
  // Raise normalised luma to a power. Midtone-weighted, so black stays black
  // and white stays white, and the amount never clips.
  Gamma = 1,
  // Additive offset scaled by the remaining headroom. Moves shadows most and
  // leaves white alone, which is what "lift" means in a grading suite.
  Lift = 2,
  // Parabolic lift, zero at both ends and strongest at mid grey. Black and
  // white stay put while the middle opens up, so a large amount cannot clip.
  Soft = 3,
};

// How the grade changes saturation.
enum class GradeChromaMode : int {
  // Blend toward luma. Uniform across the image, so a saturated pixel moves
  // as much as a muted one — which is what makes it the predictable default.
  Saturation = 0,
  // Scale by how muted the pixel already is, so colours near grey move and
  // colours that are already vivid hold back.
  Vibrance = 1,
  // Scale the chroma channels only, leaving luma exactly where it was. Use
  // this when a saturation change should not touch brightness at all.
  ChromaGain = 2,
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

  // Levels, applied to linear luma before anything else.
  //
  // Black point is the linear value mapped to black. Raising it crushes the
  // floor — everything below it goes to black and the rest is stretched to
  // fill the range. Lowering it lifts the floor instead, so black becomes a
  // dark grey. White point is the linear value mapped to white, so lowering
  // it brightens the image rather than only touching the top end.
  float black_point = 0.0f;   // [kGradeBlackPointMin, kGradeBlackPointMax]
  float white_point = 1.0f;   // [kGradeWhitePointMin, kGradeWhitePointMax]

  // Contrast, a stretch around mid grey. Positive steepens, negative flattens.
  float contrast = 0.0f;      // [-1, 1]

  // Region-weighted scales. Shadows moves the dark end and highlights the
  // bright end, each fading out before it reaches the other, so neither can
  // flatten the image on its own.
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

  // Grades applied at this pass's two model boundaries. `grade_input` runs on
  // the image the model is about to read; `grade_output` on the image it
  // produced, before that image is published.
  //
  // Not inheritable, unlike the fields below. A grade is a statement about
  // this image at this point, so carrying it into the next pass would apply
  // it twice rather than extend it.
  GradeSettings grade_input;
  GradeSettings grade_output;

  // How this pass's model edit is carried onto the source. Also not
  // inheritable, for the same reason as the grades: it describes what this
  // pass's model produced.
  ResidualSettings residual;

  // FSR RCAS sharpening applied to the EASU-upsampled working-scale images
  // before the ratio is taken. Not inheritable: like the grades, it is a
  // statement about this pass's images.
  bool rcas_enabled = false;
  float rcas_strength = 0.5f;  // [0, 1]

  // Inheritance flags. Only meaningful for pass index > 0; the root pass
  // ignores them. A true flag makes the corresponding slider use the
  // resolved value from the previous pass instead of its own raw value.
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

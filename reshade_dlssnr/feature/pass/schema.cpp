// Pass schema: one instance per pass.
//
// The pass section is instanced, so every row here describes one pass and the
// store keeps one value set per pass key. Nothing in this file knows a pass
// index: identity is the instance key.
//
// The two model grades are separate sections rather than categories of this
// one. A category is a label and never carries identity; two grade instances
// in one section would both want "black_level" and the second would collide
// with the first. As separate sections they get separate name spaces, and both
// still call the same grade_rows fragment -- the pass depends on the fragment,
// not on the grade feature.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/draw.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/pass/settings.h"
#include "project_constants.h"
#include "feature/pass/ui.h"
#include "fragments/grade_rows/rows.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

namespace {

bool NeverVisible(const void*, const ctl::ValueStore&,
                  const ctl::InstanceKey&, const std::string&) {
  DLSSNR_PROFILE_CTL_L3("NeverVisible");
  return false;
}

// Category tags. A tag is a label; consecutive rows sharing one are drawn
// under a collapsible sub-header, so it also carries the sub-header's identity
// -- which is why the two grades cannot share one.
constexpr const char* kCatModel = "Model";
constexpr const char* kCatStrength = "Strength";
constexpr const char* kCatResidualEdits = "Residual Edits";
constexpr const char* kCatTemporal = "Temporal Reprojection";

// Availability of each target stage in this game, shown dimmed in the button
// row when the stage cannot run right now. The choice stays selectable: the
// reason is in the tooltip, and the stage can become available again (a
// different upscaler, or a fix for the blocked case).
void TargetAvailability(const void*, int index,
                        ctl::EnumSetting::ItemAvailability* out) {
  out->available = true;
  out->reason = nullptr;

  // The snapshot is a copy, so the reason text has to outlive this call. One
  // buffer per stage: the draw keeps every reason pointer until it has drawn
  // the whole row.
  static char reasons[2][kStatusTextLength] = {};
  char* reason = reasons[index == static_cast<int>(InjectionStage::PreDLSS) ? 0 : 1];
  const StatusSnapshot status = AddonController::Get().GetStatus();

  const bool blocked =
      index == static_cast<int>(InjectionStage::PreDLSS) ? status.pre_dlss_blocked
      : index == static_cast<int>(InjectionStage::PostDLSS) ? status.post_dlss_blocked
                                                            : false;
  if (!blocked) return;

  std::snprintf(reason, kStatusTextLength, "%s",
                index == static_cast<int>(InjectionStage::PreDLSS)
                    ? status.pre_dlss_block_reason
                    : status.post_dlss_block_reason);
  out->available = false;
  out->reason = reason;
}

const char* const kTargetNames[] = {"Pre-DLSS", "Post-DLSS", "Backbuffer"};
const char* const kTargetTips[] = {
    "Run before the game's DLSS evaluation on its input Color resource. The "
    "game then upscales the modified image. Requires a full-resource Color "
    "input.",
    "Run after the game's DLSS evaluation on its upscaled output, before later "
    "post-processing and UI.",
    "Run on the final swapchain backbuffer at ReShade Present. Works even "
    "when the game does not use DLSS; it is not ordered with Pre/Post-DLSS "
    "passes unless those stages are available."};

const char* const kTemporalNames[] = {"Off", "Adaptive", "Fixed"};
const char* const kTemporalTips[] = {
    "Evaluate the neural model on every frame.",
    "Reuse the reprojected edit as far as per-pixel confidence allows, "
    "forcing a refresh at least every N frames where N is the Cadence "
    "slider. The effective cadence halves when the invalid pixel fraction "
    "is high.",
    "Reuse the reprojected edit for a fixed number of frames between model "
    "evaluations, regardless of confidence. The Cadence slider is the "
    "exact cadence."};

const char* const kPresetNames[] = {"A", "B", "C"};
// Presets are a name, not an enum: the C++ type is an int the model reads.
const char* const kPresetTips[] = {
    "Use model preset A. Changes are applied per evaluation and reset the "
    "model's internal history without rebuilding the feature.",
    "Use model preset B. Changes are applied per evaluation and reset the "
    "model's internal history without rebuilding the feature.",
    "Use model preset C. Changes are applied per evaluation and reset the "
    "model's internal history without rebuilding the feature."};

bool DrawGradeSections(const void*, ctl::ValueStore& store,
                       const ctl::DrawOptions& options,
                       const std::string&, const ctl::InstanceKey& key,
                       bool& changed_any) {
  DLSSNR_PROFILE_UI_L3("DrawGradeSections");
  ctl::DrawOptions nested = options;
  nested.draw_headers = true;
  nested.indent_bodies = true;
  bool changed = false;
  bool section_changed = false;
  ctl::DrawSection(store, kGradeInputSection, key, section_changed, nested);
  changed |= section_changed;
  section_changed = false;
  ctl::DrawSection(store, kGradeOutputSection, key, section_changed, nested);
  changed |= section_changed;
  changed_any |= changed;
  return changed;
}

bool DrawWorkingScaleRow(const void*, ctl::ValueStore& store,
                        const ctl::DrawOptions&, const std::string& section,
                        const ctl::InstanceKey& key, bool& changed) {
  DLSSNR_PROFILE_UI_L3("DrawWorkingScaleRow");
  const float scale = store.Read(section, key, kWorkingScale).as_float;
  const bool x2 = store.Read(section, key, kWorkingScaleX2).as_bool;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Working Scale");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("The resolution the model runs at, as a fraction of the frame. "
        "Tick x2 to double it for supersampling. Compose and the "
        "backbuffer stay at frame resolution.");
  }

  ImGui::SameLine(kSliderLabelWidth);

  const ImGuiStyle& style = ImGui::GetStyle();
  const float x2_width = ImGui::GetFrameHeight() +
                         style.ItemInnerSpacing.x +
                         ImGui::CalcTextSize("x2").x;
  const float slider_width = std::max(
      60.0f, ImGui::GetContentRegionAvail().x - x2_width -
                 kResetButtonWidth - 2.0f * style.ItemSpacing.x);
  ImGui::SetNextItemWidth(slider_width);

  float new_scale = std::clamp(scale, 0.10f, 1.0f);
  const bool scale_changed =
      ImGui::SliderFloat("##working_scale", &new_scale, 0.10f, 1.0f, "%.3f");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("The model's working resolution as a fraction of the frame.\n\n"
        "Very low values are raised to a minimum of 180p vertical "
        "resolution, so temporal reprojection always has usable history. "
        "Changing this value resets the model's internal history on its next "
        "evaluation.");
  }

  ImGui::SameLine();
  bool new_x2 = x2;
  const bool x2_changed = ImGui::Checkbox("x2##working_scale_x2", &new_x2);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (!new_x2) {
      Tip("Double the model's working resolution for supersampling. This "
          "resets the model's internal history on its next evaluation.\n\n"
          "WARNING: enabling this can freeze the game for a brief moment on "
          "the first frame after toggling, while the larger working-scale "
          "resources (working input/output, temporal history, unlifted "
          "reference) are allocated and the previously in-flight buffers "
          "retire on the GPU.\n\n"
          "Other hazards:\n"
          "  - VRAM use roughly quadruples for the working-scale resources.\n"
          "  - Model evaluation and compose are slower at 2x.\n"
          "  - The cheap model reused on reuse frames grows with the working "
          "scale, so reuse frames are more expensive too.\n"
          "  - A freeze longer than a second or two usually means the device "
          "has run out of VRAM; toggle this off to recover.");
    } else {
      Tip("Currently doubling the model's working resolution for "
          "supersampling. Uncheck to return to the slider's value. Changing "
          "this setting resets the model's internal history on its next "
          "evaluation.\n\n"
          "The first frame after unchecking can also freeze briefly while "
          "smaller resources are allocated and the previous ones retire on "
          "the GPU.");
    }
  }

  ImGui::SameLine();
  ImGui::PushID("working_scale_reset");
  const bool reset_clicked =
      ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset Working Scale and x2 to their defaults.");
  }
  ImGui::PopID();

  bool local_changed = false;
  if (scale_changed) {
    store.Write(section, key, kWorkingScale,
                ctl::Value::Float(new_scale), /*from_user=*/true);
    local_changed = true;
  }
  if (x2_changed) {
    store.Write(section, key, kWorkingScaleX2,
                ctl::Value::Bool(new_x2), /*from_user=*/true);
    local_changed = true;
  }
  if (reset_clicked) {
    store.Write(section, key, kWorkingScale,
                ctl::Value::Float(1.0f), /*from_user=*/true);
    store.Write(section, key, kWorkingScaleX2,
                ctl::Value::Bool(false), /*from_user=*/true);
    local_changed = true;
  }

  changed |= local_changed;
  return local_changed;
}

}  // namespace

}  // namespace pass

void RegisterPassSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterPassSchema");
  using namespace pass;

  ctl::Section& order = schema.AddSection(kOrderSection, ctl::SectionKind::Single);
  order.Add<ctl::IntListSetting>(kOrderSetting, "Pass Order",
                                 std::vector<int64_t>{1})
      .MinItems(1)
      .MaxItems(kMaxPasses)
      .RangeEach(1, UINT32_MAX)
      .UniqueItems()
      .VisibleWhen(&NeverVisible);

  ctl::Section& pass =
      schema.AddSection(kSection, ctl::SectionKind::Instanced);

  // The pass toggle remains an ordinary setting so it stays editable while
  // the pass is disabled. Target is a group-button enum setting.
  pass.Add<ctl::BoolSetting>(kPassEnabled, "Enable", true)
      .Tooltip("Enable or disable this pass profile. Disabled passes do not "
               "evaluate the model.")
      .VisibleWhen(&NeverVisible);
  pass.Add<ctl::EnumSetting>(kTarget, "Target", 2)
      .Items(kTargetNames, kTargetTips, 3)
      .AsGroup()
      .Availability(&TargetAvailability)
      .Tooltip("Each pass can independently target Pre-DLSS, Post-DLSS, or "
               "Backbuffer. Execution order is grouped by stage; passes "
               "within each stage follow their pass-tab order. Changing the "
               "target resets the model's internal history on its next "
               "evaluation.");

  // The tab label. A pass shows its own name; the cap matches the buffer the
  // tab bar formats into.
  pass.Add<ctl::StringSetting>(kCustomName, "Name", std::string())
      .MaxLength(63)
      .Tooltip("Label shown on this pass's tab. Empty shows the positional "
               "default, \"Pass N\".");

  // The working-scale row is drawn by DrawWorkingScaleRow so the x2 toggle
  // can sit between the slider and its reset button. Both declarations are
  // hidden from the default drawer; the hook renders the entire row.
  pass.Add<ctl::FloatSetting>(kWorkingScale, "Working Scale", 1.0f)
      .ClampRange(0.10f, 1.0f)
      .VisibleWhen(&NeverVisible)
      .Tooltip("The resolution the model runs at, as a fraction of the frame.");

  pass.Add<ctl::BoolSetting>(kWorkingScaleX2, "Working Scale x2", false)
      .VisibleWhen(&NeverVisible)
      .Tooltip("Double the model's working resolution.");

  pass.AddHookAfter(kWorkingScale, "WorkingScaleRow",
                    &DrawWorkingScaleRow).Category(nullptr);

  // --- Model -------------------------------------------------------------
  pass.Add<ctl::EnumSetting>(kPreset, "Preset", 0)
      .Items(kPresetNames, kPresetTips, 3)
      .AsGroup()
      .Category(kCatModel)
      .Tooltip("Select the model quality/behavior preset for this pass.");
  pass.Add<ctl::IntSetting>(kRenderPreset, "Render Preset", 1)
      .ClampRange(1, 3)
      .Category(kCatModel)
      .Tooltip("DLSSNR.Hint.Render.Preset value for this pass (1, 2, or 3). "
               "NGX latches this hint at feature creation, so changing it "
               "rebuilds this pass's feature.");
  pass.Add<ctl::FloatSetting>(kIntensity, "Intensity", 1.0f)
      .ClampRange(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How strongly the model is instructed to edit the image. 0 "
               "turns the model's edits off entirely.");
  pass.Add<ctl::FloatSetting>(kLocalTone, "Local Tone", 1.0f)
      .ClampRange(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How much of the model's local tone adjustments to apply.");
  pass.Add<ctl::FloatSetting>(kLocalStructure, "Local Structure", 1.0f)
      .ClampRange(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How much of the model's local structure detail to apply.");
  pass.Add<ctl::BoolSetting>(kAutoMask, "Auto Mask", true)
      .Category(kCatModel)
      .Tooltip("Lets the model detect character skin and apply its edits to it "
               "separately.");
  pass.Add<ctl::FloatSetting>(kSkinStructure, "Skin Structure", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .EnableWhen(kAutoMask)
      .InlineToggle(kAutoMask)
      .Category(kCatModel)
      .Tooltip("Controls how strongly the model applies its skin-specific "
               "edits. Disabled when the model's automatic masking option is off.");
  pass.Add<ctl::BoolSetting>(kUiCorrection, "UI Correction", true)
      .Category(kCatModel)
      .Tooltip("Reduce the model's edits where it detects UI elements.");

  // --- Strength ----------------------------------------------------------
  pass.Add<ctl::FloatSetting>(kLumaStrength, "Luma Strength", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Category(kCatStrength)
      .Tooltip("Blend between original and edited luminance.");
  pass.Add<ctl::FloatSetting>(kChromaStrength, "Chroma Strength", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Category(kCatStrength)
      .Tooltip("Blend between original and edited chroma.");

  // --- Temporal Reprojection --------------------------------------------
  pass.Add<ctl::BoolSetting>(kSyntheticMotionGuides, "Synthetic Motion Guides", false)
      .Category(kCatTemporal)
      .Tooltip("When native motion vectors are missing, estimate a current-to-previous "
               "motion field by color matching constrained by current and previous depth. "
               "This costs additional GPU work and needs a usable depth guide.");
  pass.Add<ctl::EnumSetting>(kTemporalMode, "Mode", 0)
      .Items(kTemporalNames, kTemporalTips, 3)
      .AsGroup()
      .Category(kCatTemporal)
      .Tooltip("Reuse a motion-reprojected multiplicative model edit instead "
               "of evaluating the model every frame. Requires current-frame "
               "depth and native or synthetic motion guides; uncertain history "
               "forces a fresh evaluation.");
  pass.Add<ctl::IntSetting>(kTemporalCadence, "Cadence", 2)
      .ClampRange(2, 8)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Number of frames between model evaluations. In Adaptive mode, "
               "this is the maximum; the effective cadence halves when the "
               "invalid pixel fraction is high. In Fixed mode, this is the "
               "exact cadence.");
  pass.Add<ctl::FloatSetting>(kTemporalCheapScale, "Cheap Model Scale", 0.5f)
      .ClampRange(0.1f, 1.0f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Resolution of the cheaper model used on frames between model "
               "evaluations, as a fraction of the working scale. 0.5 means "
               "50% of the working scale resolution. Lower values are faster "
               "but fill low-confidence reprojection areas with less detail.");
  pass.Add<ctl::FloatSetting>(kTemporalPhotometricAbsThreshold,
                              "Photo Abs.", 0.06f)
      .ClampRange(0.001f, 0.25f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Absolute model-input luma difference allowed between the "
               "current pixel and its motion-reprojected previous-frame match.");
  pass.Add<ctl::FloatSetting>(kTemporalPhotometricRelThreshold,
                              "Photo Rel.", 0.10f)
      .ClampRange(0.0f, 0.50f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Relative luma-difference allowance, scaled by the larger "
               "absolute current or previous luma. Added to Photo Abs.");
  pass.Add<ctl::FloatSetting>(kTemporalDepthAbsThreshold,
                              "Depth Abs.", 0.005f)
      .ClampRange(0.0001f, 0.05f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Absolute depth difference allowed after motion reprojection.");
  pass.Add<ctl::FloatSetting>(kTemporalDepthRelThreshold,
                              "Depth Rel.", 0.03f)
      .ClampRange(0.0f, 0.20f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Relative depth-difference allowance. Threshold is the larger "
               "of this allowance and Depth Abs.");
  pass.Add<ctl::FloatSetting>(kTemporalMaxDisplacement,
                              "Max Motion", 40.0f)
      .ClampRange(1.0f, 128.0f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Reject reprojection when motion exceeds this many working-" 
               "resolution pixels.");
  pass.Add<ctl::FloatSetting>(kTemporalMaxInvalidPercent,
                              "Max Invalid %", 15.0f)
      .ClampRange(1.0f, 50.0f)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Maximum invalid or photometrically mismatched pixel fraction "
               "allowed before refreshing the model.");
  pass.Add<ctl::BoolSetting>(kTemporalFp16History, "FP16 Edit History", false)
      .EnableWhen(kTemporalMode)
      .Category(kCatTemporal)
      .Tooltip("Store temporal edit/candidate textures in FP16. Reduces history "
               "bandwidth and memory with small quantization differences on reused edits.");
  // Live readout, last row of the category. The category header is the
  // collapsible; the hook only draws the body.
  pass.AddHookAfter(kTemporalFp16History, "TemporalStats", &DrawTemporalStats)
      .Category(kCatTemporal);

  // --- Advanced ----------------------------------------------------------
  pass.Add<ctl::FloatSetting>(kEditStrength, "Edit Strength", 1.0f)
      .ClampRange(0.0f, 2.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Scales the model's edit before it is carried onto the source.");
  pass.Add<ctl::FloatSetting>(kResidualChroma, "Chroma Strength", 1.0f)
      .ClampRange(0.0f, 2.0f)
      .Category(kCatResidualEdits)
      .Tooltip("How much of the edit's chroma lands relative to its "
               "brightness.");
  pass.Add<ctl::FloatSetting>(kResidualGamma, "Edit Gamma", 1.0f)
      .ClampRange(0.25f, 4.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Shapes the edit's response curve.");
  pass.Add<ctl::FloatSetting>(kResidualHighlight, "Highlight Protect", 0.0f)
      .ClampRange(0.0f, 1.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Fades the edit out above mid-to-high brightness.");
  pass.Add<ctl::FloatSetting>(kResidualShadow, "Shadow Protect", 0.0f)
      .ClampRange(0.0f, 1.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Fades the edit out below mid-to-low brightness.");

  pass.Add<ctl::FloatSetting>(kUpsampleSharpness, "Upsample Sharpness", 8.0f)
      .ClampRange(0.5f, 32.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Edge-preservation exponent for the fixed joint-bilateral 3x3 "
               "reconstruction of the model input and output. Higher values "
               "track full-resolution guide edges more sharply; lower values "
               "blend more smoothly across the low-resolution neighborhood. "
               "This does not scale the model edit.");

  pass.Add<ctl::BoolSetting>(kRatioClampsEnabled, "Ratio Clamps", true)
      .Category(kCatResidualEdits)
      .Tooltip("Bound the per-channel model residual log ratio to the "
               "[Ratio Darken, Ratio Brighten] range before shaping. "
               "Applies to both SDR and HDR composition.");
  pass.Add<ctl::FloatSetting>(kRatioEps, "Ratio Eps", 1e-3f)
      .ClampRange(1e-6f, 0.5f)
      .Category(kCatResidualEdits)
      .Tooltip("Denominator floor for the per-channel residual log ratio. "
               "Keeps the residual finite when a channel is near zero. "
               "Applies to both SDR and HDR composition.");
  pass.Add<ctl::FloatSetting>(kRatioBrighten, "Ratio Brighten", 4.0f)
      .ClampRange(1.0f, 16.0f)
      .EnableWhen(kRatioClampsEnabled)
      .Category(kCatResidualEdits)
      .Tooltip("Upper bound on the per-channel residual ratio when Ratio "
               "Clamps is enabled. Applies to both SDR and HDR composition.");
  pass.Add<ctl::FloatSetting>(kRatioDarken, "Ratio Darken", 1.0f / 16.0f)
      .ClampRange(1.0f / 256.0f, 1.0f)
      .EnableWhen(kRatioClampsEnabled)
      .Category(kCatResidualEdits)
      .Tooltip("Lower bound on the per-channel residual ratio when Ratio "
               "Clamps is enabled. Applies to both SDR and HDR composition.");

  // --- The two model grades ----------------------------------------------
  ctl::Section& grade_input =
      schema.AddSection(kGradeInputSection, ctl::SectionKind::Instanced);
  grade_input.HeaderToggle(kGradeEnabled);
  grade_input.Add<ctl::BoolSetting>(kGradeEnabled, "Grade Model Input", false)
      .Tooltip("Grade the image the model reads, before model evaluation.");
  grade_input.Add<ctl::BoolSetting>(kGradeInputApplyToPreGrade,
                                    "Apply Edit to Pre-Grade Image", false)
      .Tooltip("Measure the model edit against the actual Grade Model Input result, "
               "then apply that edit to the image before this grade section. "
               "Earlier processing, including HDR/local exposure and prior "
               "passes, remains part of the model's input context.")
      .EnableWhen(kGradeEnabled);
  grade_rows::Declare(grade_input, kGradeInputPrefix, kGradeEnabled);

  ctl::Section& grade_output =
      schema.AddSection(kGradeOutputSection, ctl::SectionKind::Instanced);
  grade_output.HeaderToggle(kGradeEnabled);
  grade_output.Add<ctl::BoolSetting>(kGradeEnabled, "Grade Model Output", false)
      .Tooltip("Grade the model produced, before it is published.");
  grade_rows::Declare(grade_output, kGradeOutputPrefix, kGradeEnabled);

  pass.AddHookAfter(kRatioDarken, "GradeBoundarySections",
                    &DrawGradeSections).Category(nullptr);
}

}  // namespace feature
}  // namespace reshade_dlssnr

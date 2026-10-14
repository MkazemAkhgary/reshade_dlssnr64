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
#include "ctl/draw.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "fragments/grade_rows/rows.h"

namespace reshade_dlssnr {
namespace feature {
namespace pass {

namespace {

// Category tags. A tag is a label; consecutive rows sharing one are drawn
// under a collapsible sub-header, so it also carries the sub-header's identity
// -- which is why the two grades cannot share one.
constexpr const char* kCatModel = "Model";
constexpr const char* kCatStrength = "Strength";
constexpr const char* kCatSoftCompression = "Advanced/Soft Compression";
constexpr const char* kCatResidualEdits = "Advanced/Residual Edits";

// Grade is the only part of the pass that the pass does not gate itself: the
// grade rows follow the grade's own enable checkbox, and the two grade
// sections are independent.
bool GradeEnabled(const void*, const ctl::ValueStore& store,
                  const ctl::InstanceKey& key, const std::string& section) {
  return store.Read(section, key, kGradeEnabled).as_bool;
}

// The model's skin-strength row is interactive only when automatic masking is
// on -- the same rule the hand-written UI applied with WithDisabled.
bool AutoMaskOn(const void*, const ctl::ValueStore& store,
                const ctl::InstanceKey& key, const std::string& section) {
  return store.Read(section, key, kAutoMask).as_bool;
}

// Soft-compression parameter rows follow its own enable checkbox.
bool SoftCompressionOn(const void*, const ctl::ValueStore& store,
                       const ctl::InstanceKey& key, const std::string& section) {
  return store.Read(section, key, kSoftCompressionEnabled).as_bool;
}

// The guided-upsample rows follow Upsample Mode == Joint Bilateral.
bool JointBilateral(const void*, const ctl::ValueStore& store,
                    const ctl::InstanceKey& key, const std::string& section) {
  return store.Read(section, key, kUpsampleMode).as_int == 1;
}

// RCAS strength follows its own enable checkbox.
bool RcasOn(const void*, const ctl::ValueStore& store,
            const ctl::InstanceKey& key, const std::string& section) {
  return store.Read(section, key, kRcasEnabled).as_bool;
}

// The ratio rows are Backbuffer-only. That is a property of this pass's target,
// which is itself a row in this section -- so the gate just reads it, exactly
// like every other "follows that checkbox" rule here.
bool BackbufferTarget(const void*, const ctl::ValueStore& store,
                      const ctl::InstanceKey& key,
                      const std::string& section) {
  return store.Read(section, key, kTarget).as_int == 2;
}

bool RcasStrengthVisible(const void*, const ctl::ValueStore& store,
                         const ctl::InstanceKey& key,
                         const std::string& section) {
  return BackbufferTarget(nullptr, store, key, section) &&
         RcasOn(nullptr, store, key, section);
}

const char* const kTargetNames[] = {"Pre-DLSS", "Post-DLSS", "Backbuffer"};
const char* const kTargetTips[] = {
    "Run before the game's DLSS evaluation on its input Color resource. The "
    "game then upscales the modified image. Requires a full-resource Color "
    "input.",
    "Run after the game's DLSS evaluation on its upscaled output, before later "
    "post-processing and UI.",
    "Run on the final swapchain backbuffer, after DLSS-stage passes."};

const char* const kPresetNames[] = {"A", "B", "C"};
// Presets are a name, not an enum: the C++ type is an int the model reads.
const char* const kPresetTips[] = {
    "Use model preset A. Preset changes are applied per evaluation and do not "
    "rebuild the feature.",
    "Use model preset B. Preset changes are applied per evaluation and do not "
    "rebuild the feature.",
    "Use model preset C. Preset changes are applied per evaluation and do not "
    "rebuild the feature."};

const char* const kUpsampleNames[] = {"EASU", "Joint Bilateral"};
const char* const kUpsampleTips[] = {
    "FSR 1.0 EASU upsampling. The original upsampler, designed for roughly 2x "
    "reconstruction. At working scales far from that it has less source detail "
    "to work with and the composed image can look blocky.",
    "Joint bilateral upsampling guided by the full-resolution original. Each "
    "output pixel is a weighted average of the nearby low-resolution samples, "
    "weighted by guide-luma similarity. Preserves the model's per-channel edit "
    "at any working scale."};

const char* const kKernelNames[] = {"3x3", "5x5", "7x7"};
const char* const kKernelTips[] = {
    "3x3 kernel. Nine samples per output pixel. Lowest cost, good for typical "
    "working scales.",
    "5x5 kernel. Twenty-five samples. Wider spatial support, smoother "
    "reconstruction across curves and diagonals, and weaker sample-grid "
    "modulation at very low working scales.",
    "7x7 kernel. Forty-nine samples. Further smoothing at increasing cost; the "
    "outer ring contributes little unless the scene has large, "
    "softly-varying low-resolution features."};

bool DrawGradeSections(const void*, ctl::ValueStore& store,
                       const ctl::DrawOptions& options,
                       const std::string&, const ctl::InstanceKey& key,
                       bool& changed_any) {
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

}  // namespace

}  // namespace pass

void RegisterPassSchema(ctl::Schema& schema) {
  using namespace pass;

  ctl::Section& pass =
      schema.AddSection(kSection, ctl::SectionKind::Instanced);

  // The pass toggle remains an ordinary setting so it stays editable while
  // the pass is disabled. Target is a group-button enum setting.
  pass.Add<ctl::BoolSetting>(kPassEnabled, "Enable", true)
      .Tooltip("Enable or disable this pass profile. Disabled passes do not "
               "evaluate the model.");
  pass.Add<ctl::EnumSetting>(kTarget, "Target", 2)
      .Items(kTargetNames, kTargetTips, 3)
      .AsGroup()
      .Tooltip("Each pass can independently target Pre-DLSS, Post-DLSS, or "
               "Backbuffer. Execution order is grouped by stage; passes "
               "within each stage follow their pass-tab order.");

  // The tab label. Not inheritable: a pass shows its own name, never its
  // predecessor's, and inheriting it would rename a pass the user did not
  // touch. The cap matches the buffer the tab bar formats into.
  pass.Add<ctl::StringSetting>(kCustomName, "Name", std::string())
      .MaxLength(63)
      .Tooltip("Label shown on this pass's tab. Empty shows the positional "
               "default, \"Pass N\".");

  pass.Add<ctl::FloatSetting>(kWorkingScale, "Working Scale", 1.0f)
      .Range(0.10f, 1.0f)
      .Tooltip("The resolution the model runs at, as a fraction of the frame.");

  // --- Model -------------------------------------------------------------
  pass.Add<ctl::EnumSetting>(kPreset, "Preset", 0)
      .Items(kPresetNames, kPresetTips, 3)
      .AsGroup()
      .Category(kCatModel)
      .Tooltip("Select the model quality/behavior preset for this pass.");
  pass.Add<ctl::FloatSetting>(kIntensity, "Intensity", 1.0f)
      .Range(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How strongly the model is instructed to edit the image. 0 "
               "turns the model's edits off entirely.");
  pass.Add<ctl::FloatSetting>(kLocalTone, "Local Tone", 1.0f)
      .Range(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How much of the model's local tone adjustments to apply.");
  pass.Add<ctl::FloatSetting>(kLocalStructure, "Local Structure", 1.0f)
      .Range(0.0f, 2.0f)
      .Category(kCatModel)
      .Tooltip("How much of the model's local structure detail to apply.");
  pass.Add<ctl::BoolSetting>(kAutoMask, "Auto Mask", true)
      .Category(kCatModel)
      .Tooltip("Lets the model detect character skin and apply its edits to it "
               "separately.");
  pass.Add<ctl::FloatSetting>(kSkinStructure, "Skin Structure", 1.0f)
      .Range(0.0f, 1.0f)
      .EnableWhen(&AutoMaskOn)
      .Category(kCatModel)
      .Tooltip("Controls how strongly the model applies its skin-specific "
               "edits. Disabled when the model's automatic masking option is "
               "off or this value is inherited.");
  pass.Add<ctl::BoolSetting>(kUiCorrection, "UI Correction", true)
      .Category(kCatModel)
      .Tooltip("Reduce the model's edits where it detects UI elements.");

  // --- Strength ----------------------------------------------------------
  pass.Add<ctl::FloatSetting>(kLumaStrength, "Luma Strength", 1.0f)
      .Range(0.0f, 1.0f)
      .Category(kCatStrength)
      .Tooltip("Blend between original and edited luminance.");
  pass.Add<ctl::FloatSetting>(kChromaStrength, "Chroma Strength", 1.0f)
      .Range(0.0f, 1.0f)
      .Category(kCatStrength)
      .Tooltip("Blend between original and edited chroma.");

  // --- Advanced ----------------------------------------------------------
  pass.Add<ctl::BoolSetting>(kSoftCompressionEnabled, "Soft Compression", false)
      .Category(kCatSoftCompression)
      .CategoryToggle()
      .Tooltip("Enable rational soft clamp of log residuals.");
  pass.Add<ctl::FloatSetting>(kSoftMaxBrighten, "Max Brighten", 0.5f)
      .Range(0.0f, 2.0f)
      .EnableWhen(&SoftCompressionOn)
      .Category(kCatSoftCompression)
      .Tooltip("Maximum allowed log-luma brightening.");
  pass.Add<ctl::FloatSetting>(kSoftMaxDarken, "Max Darken", 0.5f)
      .Range(0.0f, 2.0f)
      .EnableWhen(&SoftCompressionOn)
      .Category(kCatSoftCompression)
      .Tooltip("Maximum allowed log-luma darkening.");
  pass.Add<ctl::FloatSetting>(kSoftMaxChroma, "Max Chroma", 0.5f)
      .Range(0.0f, 2.0f)
      .EnableWhen(&SoftCompressionOn)
      .Category(kCatSoftCompression)
      .Tooltip("Maximum allowed log-chroma change per channel.");
  pass.Add<ctl::FloatSetting>(kSoftKnee, "Knee", 1.0f)
      .Range(0.0f, 4.0f)
      .EnableWhen(&SoftCompressionOn)
      .Category(kCatSoftCompression)
      .Tooltip("Controls how early compression starts (linear range).");

  pass.Add<ctl::FloatSetting>(kEditStrength, "Edit Strength", 1.0f)
      .Range(0.0f, 2.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Scales the model's edit before it is carried onto the source.");
  pass.Add<ctl::FloatSetting>(kResidualChroma, "Chroma Strength", 1.0f)
      .Range(0.0f, 1.0f)
      .Category(kCatResidualEdits)
      .Tooltip("How much of the edit's chroma lands relative to its "
               "brightness.");
  pass.Add<ctl::FloatSetting>(kResidualGamma, "Edit Gamma", 1.0f)
      .Range(0.25f, 4.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Shapes the edit's response curve.");
  pass.Add<ctl::FloatSetting>(kResidualHighlight, "Highlight Protect", 0.0f)
      .Range(0.0f, 1.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Fades the edit out above mid-to-high brightness.");
  pass.Add<ctl::FloatSetting>(kResidualShadow, "Shadow Protect", 0.0f)
      .Range(0.0f, 1.0f)
      .Category(kCatResidualEdits)
      .Tooltip("Fades the edit out below mid-to-low brightness.");

  pass.Add<ctl::EnumSetting>(kUpsampleMode, "Upsample Mode", 0)
      .Items(kUpsampleNames, kUpsampleTips, 2)
      .AsGroup()
      .Category(kCatResidualEdits)
      .Tooltip("Which algorithm reconstructs full resolution from the "
               "working-scale model input and output. EASU is the original "
               "FSR 1.0 path; Joint Bilateral uses the full-resolution "
               "original as an edge guide.");
  pass.Add<ctl::FloatSetting>(kUpsampleSharpness, "Upsample Sharpness", 8.0f)
      .Range(0.5f, 32.0f)
      .VisibleWhen(&JointBilateral)
      .Category(kCatResidualEdits)
      .Tooltip("Edge-preservation exponent for the guided upsample of the "
               "model's input and output. Higher values track the "
               "full-resolution image's edges more sharply; lower values blend "
               "more smoothly across the low-resolution neighborhood. This is "
               "an edge-preservation exponent, not a strength multiplier: it "
               "does not amplify or attenuate the model's edit.");
  pass.Add<ctl::EnumSetting>(kUpsampleKernel, "Upsample Kernel", 0)
      .Items(kKernelNames, kKernelTips, 3)
      .AsGroup()
      .VisibleWhen(&JointBilateral)
      .Category(kCatResidualEdits)
      .Tooltip("Spatial extent of the joint-bilateral kernel. The spatial "
               "sigma is derived from the kernel size, so the falloff shape is "
               "the same across sizes and only the number of contributing taps "
               "changes.");

  // Ratio rows apply to Backbuffer passes only, which is a value in this same
  // section -- so the gate reads it exactly like every other "follows that
  // checkbox" rule, with no hook.
  pass.Add<ctl::FloatSetting>(kRatioEps, "Ratio Eps", 1e-2f)
      .Range(1e-6f, 0.5f)
      .VisibleWhen(&BackbufferTarget)
      .Category(kCatResidualEdits)
      .Tooltip("Backbuffer passes only: denominator floor for the per-channel "
               "ratio. This has no effect on Pre-DLSS or Post-DLSS passes.");
  pass.Add<ctl::FloatSetting>(kRatioBrighten, "Ratio Brighten", 64.0f)
      .Range(1.0f, 256.0f)
      .VisibleWhen(&BackbufferTarget)
      .Category(kCatResidualEdits)
      .Tooltip("Backbuffer passes only: upper bound on per-channel ratio "
               "brightening. This has no effect on Pre-DLSS or Post-DLSS "
               "passes.");
  pass.Add<ctl::FloatSetting>(kRatioDarken, "Ratio Darken", 1.0f / 16.0f)
      .Range(1.0f / 256.0f, 1.0f)
      .VisibleWhen(&BackbufferTarget)
      .Category(kCatResidualEdits)
      .Tooltip("Backbuffer passes only: lower bound on per-channel ratio "
               "darkening. This has no effect on Pre-DLSS or Post-DLSS "
               "passes.");

  pass.Add<ctl::BoolSetting>(kRcasEnabled, "Sharpening", false)
      .VisibleWhen(&BackbufferTarget)
      .Category(kCatResidualEdits)
      .Tooltip("Sharpen the model's upsampled output with FSR RCAS.");
  pass.Add<ctl::FloatSetting>(kRcasStrength, "Strength", 0.5f)
      .Range(0.0f, 1.0f)
      .VisibleWhen(&RcasStrengthVisible)
      .Category(kCatResidualEdits)
      .Tooltip("How aggressively to sharpen.");

  // --- The two model grades ----------------------------------------------
  ctl::Section& grade_input =
      schema.AddSection(kGradeInputSection, ctl::SectionKind::Instanced);
  grade_input.HeaderToggle(kGradeEnabled);
  grade_input.Add<ctl::BoolSetting>(kGradeEnabled, "Grade Model Input", false)
      .Tooltip("Grade the image the model reads, before model evaluation.");
  grade_rows::Declare(grade_input, kGradeInputPrefix, &GradeEnabled);

  ctl::Section& grade_output =
      schema.AddSection(kGradeOutputSection, ctl::SectionKind::Instanced);
  grade_output.HeaderToggle(kGradeEnabled);
  grade_output.Add<ctl::BoolSetting>(kGradeEnabled, "Grade Model Output", false)
      .Tooltip("Grade the model produced, before it is published.");
  grade_rows::Declare(grade_output, kGradeOutputPrefix, &GradeEnabled);

  pass.AddHookAfter(kRcasStrength, "GradeBoundarySections",
                    &DrawGradeSections).Category(nullptr);
}

}  // namespace feature
}  // namespace reshade_dlssnr

// HDR schema: source preparation and the three exposure layers.
//
// One translation unit per feature, declared whole here. Game and Adaptive
// Exposure have histogram filters; Local Exposure uses spatial-neighborhood
// controls only.
//
// The tab bar and the diagnostics block are hooks: they are rendering, not
// settings, and a hook's identity is data so it is declared where it belongs.

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/hdr/settings.h"
#include "feature/hdr/ui.h"
#include "fragments/hdr_filter_rows/rows.h"

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

namespace {

const char* const kFormatNames[] = {"Auto", "SDR", "scRGB", "HDR10"};
const char* const kFormatTips[] = {
    "Detect the format automatically from the active Pre/Post-DLSS source.",
    "Treat the source as SDR sRGB.",
    "Treat the source as linear scRGB (1.0 = 80 nits).",
    "Treat the source as HDR10 PQ (ST.2084)."};

constexpr const char* kCatAdvanced = "Advanced";
constexpr const char* kCatDiagnostics = "Diagnostics";

// The three layer toggles are the gates for everything after them, so they are
// declared once and their predicates read the section they are drawn in.
bool LayerEnabled(const void* context, const ctl::ValueStore& store,
                  const ctl::InstanceKey& key, const std::string& section) {
  const char* toggle = static_cast<const char*>(context);
  return store.Read(section, key, toggle).as_bool;
}

}  // namespace

}  // namespace hdr

void RegisterHdrSchema(ctl::Schema& schema) {
  using namespace hdr;

  // --- HDR ---------------------------------------------------------------
  ctl::Section& hdr = schema.AddSection(kSection, ctl::SectionKind::Single);
  hdr.HeaderToggle(kEnable);
  hdr.Add<ctl::BoolSetting>(kEnable, "Enable", true)
      .Tooltip("Prepares the current Pre-DLSS or Post-DLSS source before the "
               "first pass. Backbuffer-only configurations do not use HDR "
               "measurement.");
  hdr.Add<ctl::FloatSetting>(kHighlightHeadroom, "Highlight Headroom", 1.0f)
      .Range(0.25f, 16.0f)
      .Tooltip("Scales the effective HDR peak used by BT.2446C. This does not "
               "change how exposure is interpreted.");

  hdr.Add<ctl::EnumSetting>(kFormat, "Format", 0)
      .Items(kFormatNames, kFormatTips, 4)
      .AsGroup()
      .Category(kCatAdvanced)
      .Tooltip("How the source image is encoded. Auto detects from the DLSS "
               "output's DXGI format.");
  hdr.Add<ctl::IntSetting>(kMeasureInterval, "Measure Interval", 4)
      .Range(1, 32)
      .Category(kCatAdvanced)
      .Tooltip("How often exposure histograms are measured, in frames.");
  hdr.Add<ctl::BoolSetting>(kFasterMetering, "Faster HDR Metering", false)
      .Category(kCatAdvanced)
      .Tooltip("Sample one pixel from each 2x2 block during histogram metering. "
               "Lower GPU cost, but small isolated highlights may be missed.");
  hdr.Add<ctl::FloatSetting>(kBt2446K1, "BT.2446C k1", 0.83802f)
      .Range(0.001f, 2.0f)
      .Category(kCatAdvanced)
      .Tooltip("Linear-region slope.");
  hdr.Add<ctl::FloatSetting>(kBt2446K2, "BT.2446C k2", 15.09968f)
      .Range(0.001f, 32.0f)
      .Category(kCatAdvanced)
      .Tooltip("Log-region slope.");
  hdr.Add<ctl::FloatSetting>(kBt2446K3, "BT.2446C k3", 0.74204f)
      .Range(0.001f, 1.0f)
      .Category(kCatAdvanced)
      .Tooltip("Reference construction parameter retained for parity/trace.");
  hdr.Add<ctl::FloatSetting>(kBt2446K4, "BT.2446C k4", 78.99439f)
      .Range(0.001f, 200.0f)
      .Category(kCatAdvanced)
      .Tooltip("Log-region intercept in nits.");
  hdr.Add<ctl::FloatSetting>(kBt2446Crosstalk, "Crosstalk", 0.04f)
      .Range(0.0f, 0.33f)
      .Category(kCatAdvanced)
      .Tooltip("BT.2446C pre-desaturation amount.");
  hdr.Add<ctl::FloatSetting>(kGainMinLog2, "Gain Min", -4.0f)
      .Range(-32.0f, 0.0f)
      .Category(kCatAdvanced)
      .Tooltip("Configured log2 gain metadata minimum.");
  hdr.Add<ctl::FloatSetting>(kGainMaxLog2, "Gain Max", 6.0f)
      .Range(0.0f, 32.0f)
      .Category(kCatAdvanced)
      .Tooltip("Configured log2 gain metadata maximum.");

  // Layer collapsibles are rendered by the HdrLayers hook (defined in
  // feature/hdr/ui.cpp). Category(nullptr) closes the Advanced group so
  // the layers render at the top level of the HDR section.
  hdr.AddHookAfter(kGainMaxLog2, "HdrLayers", &DrawHdrLayers)
      .Category(nullptr);

  hdr.Add<ctl::BoolSetting>(kDetailedStatistics, "Detailed HDR Statistics", false)
      .Category(kCatDiagnostics)
      .Tooltip("Collect full-frame gain-map range and excursion statistics. Adds two GPU dispatches when enabled.");
  hdr.AddHookAfter(kDetailedStatistics, "HdrDiagnostics", &DrawHdrDiagnostics)
      .Category(kCatDiagnostics);

  // --- HDR Game Exposure -------------------------------------------------
  ctl::Section& game =
      schema.AddSection(kGameSection, ctl::SectionKind::Single);
  game.Add<ctl::BoolSetting>(kGameEnabled, "Game Exposure", true)
      .Tooltip("Use the game's NGX ExposureTexture as the base exposure for "
               "the stack.");
  game.Add<ctl::FloatSetting>(kGameBiasStops, "Exposure Bias", 0.0f)
      .Range(-4.0f, 4.0f)
      .Tooltip("Manual bias relative to the game's exposure, in stops. Zero "
               "leaves the game's value untouched.")
      .EnableWhen(&LayerEnabled, kGameEnabled);
  hdr_filter_rows::Declare(game, "Filters");

  // --- HDR Adaptive Exposure ---------------------------------------------
  ctl::Section& adaptive =
      schema.AddSection(kAdaptiveSection, ctl::SectionKind::Single);
  adaptive.Add<ctl::BoolSetting>(kAdaptiveEnabled, "Adaptive Exposure", true)
      .Tooltip("Use histogram-derived global exposure or a bounded correction "
               "over Game Exposure.");
  adaptive.Add<ctl::FloatSetting>(kAdaptiveMidtone, "Midtone Target", 0.18f)
      .Range(0.01f, 0.5f)
      .Tooltip("Target luminance for the selected population.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedUp, "Brighten Speed", 3.0f)
      .Range(0.0f, 20.0f)
      .Tooltip("Maximum exposure increase in stops per second.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedDown, "Darken Speed", 1.0f)
      .Range(0.0f, 20.0f)
      .Tooltip("Maximum exposure decrease in stops per second.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveStrengthOverGame, "Strength Over Game",
                              0.0f)
      .Range(0.0f, 1.0f)
      .Tooltip("When Game Exposure is active, add this fraction of the "
               "adaptive target's stop correction. With Game disabled, "
               "Adaptive is the base exposure.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveMaxCorrection, "Max Correction Stops",
                              0.75f)
      .Range(0.0f, 3.0f)
      .Tooltip("Maximum adjustment around Game Exposure when both layers are "
               "enabled.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  hdr_filter_rows::Declare(adaptive, "Filters");

  // --- HDR Local Exposure ------------------------------------------------
  ctl::Section& local = schema.AddSection(kLocalSection, ctl::SectionKind::Single);
  local.Add<ctl::BoolSetting>(kLocalEnabled, "Local Exposure", false)
      .Tooltip("Apply spatially varying exposure correction after the global "
               "exposure layers.");
  local.Add<ctl::FloatSetting>(kLocalStrength, "Strength", 0.50f)
      .Range(0.0f, 1.0f)
      .Tooltip("Blend factor for the spatial exposure adjustment.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMaxBrighten, "Max Brighten", 1.75f)
      .Range(0.0f, 4.0f)
      .Tooltip("Maximum local brightening in stops.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMaxDarken, "Max Darken", 1.75f)
      .Range(0.0f, 4.0f)
      .Tooltip("Maximum local darkening in stops.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMidtone, "Local Midtone", 0.18f)
      .Range(0.01f, 0.5f)
      .Tooltip("Reference luminance for the local base/detail operator.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::BoolSetting>(kLocalPrecomputedLuma, "Precomputed Luma", false)
      .Tooltip("Precompute HDR-to-SDR luminance once per source pixel instead of "
               "repeating the transfer at every local-exposure tap. Slightly "
               "changes interpolation order; useful for performance comparison.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::BoolSetting>(kLocalFp16Reference, "FP16 Reference", false)
      .Tooltip("Store only the unlifted working-resolution composition reference "
               "in FP16. It does not change NGX input/output or the HDR carrier; "
               "small reference quantization differences are possible.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalRestoreGlobalExposure,
                               "Restore Global Exposure", 0.0f)
      .Range(0.0f, 1.0f)
      .Tooltip("How much of the local-exposure multiplier to undo on the model "
               "output, in linear light, before the Output Grade. 1 restores "
               "the full global exposure scale and preserves chromaticity; "
               "0 leaves the model output untouched.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);

}

}  // namespace feature
}  // namespace reshade_dlssnr

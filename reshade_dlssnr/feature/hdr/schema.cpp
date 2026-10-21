// HDR schema: source preparation and the three exposure layers.
//
// One translation unit per feature, declared whole here. Only Adaptive
// Exposure has histogram filters; Game Exposure is a scalar bias on the
// game's NGX ExposureTexture, and Local Exposure uses spatial-neighborhood
// controls only.
//
// The layer collapsibles and the diagnostics block are hooks: they are
// rendering, not settings, and a hook's identity is data so it is declared
// where it belongs.

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
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

const char* const kMeteringDensityNames[] = {
    "Full", "1/4", "1/16", "1/64"};
const char* const kMeteringDensityTips[] = {
    "Sample every pixel. Full-density histogram, highest accuracy.",
    "Sample every 2nd pixel in each axis (1/4 of the frame). "
    "Small isolated highlights may be missed.",
    "Sample every 4th pixel in each axis (1/16 of the frame). "
    "Isolated highlights and fine detail may be missed.",
    "Sample every 8th pixel in each axis (1/64 of the frame). "
    "Suitable only for roughly-uniform content."};

constexpr const char* kCatAdvanced = "Advanced";
constexpr const char* kCatDiagnostics = "Diagnostics";

// The three layer toggles are the gates for everything after them, so they are
// declared once and their predicates read the section they are drawn in.
bool LayerEnabled(const void* context, const ctl::ValueStore& store,
                  const ctl::InstanceKey& key, const std::string& section) {
  DLSSNR_PROFILE_CTL_L3("LayerEnabled");
  const char* toggle = static_cast<const char*>(context);
  return store.Read(section, key, toggle).as_bool;
}

}  // namespace

}  // namespace hdr

void RegisterHdrSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterHdrSchema");
  using namespace hdr;

  // --- HDR ---------------------------------------------------------------
  ctl::Section& hdr = schema.AddSection(kSection, ctl::SectionKind::Instanced);
  hdr.HeaderToggle(kEnable);
  hdr.Add<ctl::BoolSetting>(kEnable, "Enable", true)
      .Tooltip("Meters the HDR image immediately before this pass. Later passes "
               "measure the image composed by the previous pass unless metering "
               "reuse is enabled. Backbuffer passes use SDR composition.");
  hdr.Add<ctl::FloatSetting>(kHighlightHeadroom, "Highlight Headroom", 1.0f)
      .ClampRange(0.25f, 16.0f)
      .Tooltip("Scales the effective HDR peak used by BT.2446C. This does not "
               "change how exposure is interpreted.");

  hdr.Add<ctl::EnumSetting>(kFormat, "Format", 0)
      .Items(kFormatNames, kFormatTips, 4)
      .AsGroup()
      .Category(kCatAdvanced)
      .Tooltip("How the source image is encoded. Auto detects from the DLSS "
               "output's DXGI format.");
  hdr.Add<ctl::IntSetting>(kMeasureInterval, "Measure Interval", 4)
      .ClampRange(1, 32)
      .Category(kCatAdvanced)
      .Tooltip("How often exposure histograms are measured, in frames.");
  hdr.Add<ctl::EnumSetting>(kMeteringDensity, "Metering Density", 0)
      .Items(kMeteringDensityNames, kMeteringDensityTips, 4)
      .Category(kCatAdvanced)
      .Tooltip("How many pixels the HDR histogram samples. Full density "
               "captures every pixel; coarser densities sample fewer "
               "pixels at lower GPU cost.");
  hdr.Add<ctl::BoolSetting>(kReusePreviousPassMeasurement,
                            "Reuse Previous HDR Pass Metering", false)
      .Category(kCatAdvanced)
      .Tooltip("For later passes in the same Pre-DLSS or Post-DLSS chain, reuse "
               "the previous pass's HDR measurement instead of dispatching a "
               "new histogram. The first pass still measures the game's source.");
  hdr.Add<ctl::FloatSetting>(kBt2446K1, "BT.2446C k1", 0.83802f)
      .ClampRange(0.001f, 2.0f)
      .Category(kCatAdvanced)
      .Tooltip("Linear-region slope: Y_SDR = k1 * Y_HDR for content below "
               "the inflection point. Also the slope the log branch matches "
               "at the knee.");
  hdr.Add<ctl::FloatSetting>(kBt2446K3, "BT.2446C k3", 0.74204f)
      .ClampRange(0.0f, 0.95f)
      .Category(kCatAdvanced)
      .Tooltip("Shoulder compression strength. Larger values pull the log "
               "branch down harder, darkening highlights more. k2 and k4 "
               "are derived from this so the curve stays smooth at the "
               "knee.");
  hdr.Add<ctl::FloatSetting>(kBt2446Ip, "BT.2446C Inflection", 94.26f)
      .Range(10.0f, 4000.0f)
      .ClampRange(0.001f, 10000.0f)
      .Category(kCatAdvanced)
      .Tooltip("Knee position in the BT.2446C curve domain. Content above "
               "this point is log-compressed; content below is linear. The "
               "slider covers the useful range for real HDR content. Values "
               "outside it can still be typed: 10000 disables compression.");
  hdr.AddHookAfter(kBt2446Ip, "Bt2446Derived", &DrawBt2446Derived)
      .Category(kCatAdvanced);
  hdr.Add<ctl::FloatSetting>(kBt2446Crosstalk, "Crosstalk", 0.04f)
      .ClampRange(0.0f, 0.33f)
      .Category(kCatAdvanced)
      .Tooltip("BT.2446C pre-desaturation amount.");
  hdr.Add<ctl::FloatSetting>(kGainMinLog2, "Gain Min", -4.0f)
      .ClampRange(-32.0f, 0.0f)
      .Category(kCatAdvanced)
      .Tooltip("Configured log2 gain metadata minimum.");
  hdr.Add<ctl::FloatSetting>(kGainMaxLog2, "Gain Max", 6.0f)
      .ClampRange(0.0f, 32.0f)
      .Category(kCatAdvanced)
      .Tooltip("Configured log2 gain metadata maximum.");

  // Layer collapsibles are rendered by the HdrLayers hook for this pass key.
  // Category(nullptr) closes Advanced so the layers remain top-level rows.
  hdr.AddHookAfter(kGainMaxLog2, "HdrLayers", &DrawHdrLayers)
      .Category(nullptr);

  hdr.Add<ctl::BoolSetting>(kDetailedStatistics, "Detailed HDR Statistics", false)
      .Category(kCatDiagnostics)
      .Tooltip("Collect full-frame gain-map range and excursion statistics. Adds two GPU dispatches when enabled.");
  hdr.AddHookAfter(kDetailedStatistics, "HdrDiagnostics", &DrawHdrDiagnostics)
      .Category(kCatDiagnostics);

  // --- HDR Game Exposure -------------------------------------------------
  ctl::Section& game =
      schema.AddSection(kGameSection, ctl::SectionKind::Instanced);
  game.HeaderToggle(kGameEnabled);
  game.Add<ctl::BoolSetting>(kGameEnabled, "Game Exposure", true)
      .Tooltip("Use the game's NGX ExposureTexture as the base exposure. "
               "If the game does not provide one, this layer falls through "
               "and Adaptive Exposure can supply the base instead.");
  game.Add<ctl::FloatSetting>(kGameBiasStops, "Exposure Bias", 0.0f)
      .ClampRange(-4.0f, 4.0f)
      .Tooltip("Manual bias relative to the game's exposure, in stops. Zero "
               "leaves the game's value untouched.")
      .EnableWhen(&LayerEnabled, kGameEnabled);

  // --- HDR Adaptive Exposure ---------------------------------------------
  ctl::Section& adaptive =
      schema.AddSection(kAdaptiveSection, ctl::SectionKind::Instanced);
  adaptive.HeaderToggle(kAdaptiveEnabled);
  adaptive.Add<ctl::BoolSetting>(kAdaptiveEnabled, "Adaptive Exposure", false)
      .Tooltip("With Game Exposure off, provides global exposure from a "
               "histogram measurement. With Game Exposure on, applies "
               "Strength Over Game as a bounded correction (no effect at "
               "0). Has no effect until the first measurement arrives.");
  adaptive.Add<ctl::FloatSetting>(kAdaptiveMidtone, "Midtone Target", 0.18f)
      .ClampRange(0.01f, 0.5f)
      .Tooltip("Target luminance for the selected population.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedUp, "Brighten Speed", 3.0f)
      .ClampRange(0.0f, 20.0f)
      .Tooltip("Maximum exposure increase in stops per second.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedDown, "Darken Speed", 1.0f)
      .ClampRange(0.0f, 20.0f)
      .Tooltip("Maximum exposure decrease in stops per second.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveStrengthOverGame, "Strength Over Game",
                              0.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("When Game Exposure is active, add this fraction of the "
               "adaptive target's stop correction. With Game disabled, "
               "Adaptive is the base exposure.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveMaxCorrection, "Max Correction Stops",
                              0.75f)
      .ClampRange(0.0f, 3.0f)
      .Tooltip("Maximum adjustment around Game Exposure when both layers are "
               "enabled.")
      .EnableWhen(&LayerEnabled, kAdaptiveEnabled);
  hdr_filter_rows::Declare(adaptive, "Filters");

  // --- HDR Local Exposure ------------------------------------------------
  ctl::Section& local = schema.AddSection(kLocalSection, ctl::SectionKind::Instanced);
  local.HeaderToggle(kLocalEnabled);
  local.Add<ctl::BoolSetting>(kLocalEnabled, "Local Exposure", false)
      .Tooltip("Apply spatially varying exposure correction after the Game "
               "and Adaptive layers.");
  local.Add<ctl::FloatSetting>(kLocalStrength, "Strength", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Overall amount of local exposure correction.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMaxBrighten, "Max Brighten", 2.50f)
      .ClampRange(0.0f, 8.0f)
      .Tooltip("Maximum shadow lift in stops. At full bright-detail protection, pixels above "
               "the midtone pivot are not lifted even when their neighborhood is dark.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMaxDarken, "Max Darken", 2.50f)
      .ClampRange(0.0f, 8.0f)
      .Tooltip("Maximum highlight reduction in stops. At full dark-detail protection, pixels below "
               "the pivot are not darkened even when their neighborhood is bright.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalMidtone, "Local Midtone", 0.18f)
      .ClampRange(0.01f, 0.5f)
      .Tooltip("Target luminance pivot in the model's SDR domain. Pixels near "
               "this pivot receive little correction; shadows move up and "
               "highlights move down toward it.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalShadowStrength, "Shadow Lift", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Independent multiplier for shadow lifting. Set to zero to leave "
               "dark regions where they are while still reducing highlights.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalHighlightStrength, "Highlight Reduction", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Independent multiplier for highlight reduction. Set to zero to "
               "leave bright regions where they are while still lifting shadows.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::BoolSetting>(kLocalRestoreEnabled, "Restore Model Exposure", false)
      .Tooltip("Apply a correction to the model output after evaluation. When "
               "disabled, the model's reconstructed local shading is left alone.")
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalRestoreMode, "Restore Spatial Scale", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Controls the spatial scale of exposure restoration: 0 = exact "
               "per-pixel cancellation of the local lift, 1 = uniform global "
               "correction, with log-space blending between them. Global restore "
               "retains more of the model's reconstructed brightness relations.")
      .EnableWhen(&LayerEnabled, kLocalRestoreEnabled);
  local.Add<ctl::FloatSetting>(kLocalRestoreStrength, "Restore Strength", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Amount of exposure restoration: 0 = no correction, 1 = full correction. "
               "Intermediate values blend the selected restoration in log-exposure space.")
      .EnableWhen(&LayerEnabled, kLocalRestoreEnabled);
  local.Add<ctl::FloatSetting>(kLocalRadius, "Local Radius", 32.0f)
      .ClampRange(4.0f, 128.0f)
      .Tooltip("Radius in source pixels. Larger radii respond to "
               "broader dark/bright regions instead of individual pixel noise.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::IntSetting>(kLocalSamples, "Neighborhood Samples", 12)
      .ClampRange(4, 48)
      .Tooltip("Requested spatial taps in the local log-luminance estimate. The effective "
               "count rounds to groups of four to keep sampling spatially balanced.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalShadowRange, "Shadow Transition", 1.0f)
      .ClampRange(0.25f, 6.0f)
      .Tooltip("Stops from the midtone pivot over which shadow lift ramps to full "
               "strength. A smaller value acts more aggressively on dark regions.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalHighlightRange, "Highlight Transition", 1.0f)
      .ClampRange(0.25f, 6.0f)
      .Tooltip("Stops above the midtone pivot over which highlight reduction "
               "ramps to full strength. A smaller value acts more aggressively.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalHighlightProtection, "Bright Detail Protection", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Controls whether a dark neighborhood may lift pixels above the pivot. "
               "1 keeps bright spots anchored; 0 allows correction across the pivot.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalShadowProtection, "Dark Detail Protection", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Controls whether a bright neighborhood may darken pixels below the pivot. "
               "1 keeps dark details anchored; 0 allows correction across the pivot.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalDetailRetention, "Detail Retention", 1.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Retains the pixel's log-luminance detail relative to its local "
               "base. 1 preserves all relative detail; lower values compress it.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
  local.Add<ctl::FloatSetting>(kLocalEdgeProtection, "Edge Protection", 0.5f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Reduces the influence of samples across strong log-luminance edges. "
               "0 uses spatial weights only; higher values reduce halos but can "
               "make adaptation more local.")
      .Category(kCatAdvanced)
      .EnableWhen(&LayerEnabled, kLocalEnabled);
}

}  // namespace feature
}  // namespace reshade_dlssnr

// HDR schema: source preparation and the two global exposure layers.
//
// One translation unit per feature, declared whole here. Only Adaptive
// Exposure has histogram filters; Game Exposure is a scalar bias on the
// game's NGX ExposureTexture. Local Exposure is a section of its own
// (feature/local_exposure), because it needs no HDR input.
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
#include "feature/pass/settings.h"
#include "fragments/hdr_filter_rows/rows.h"
#include "state/settings.h"

#define LOC_NAMESPACE "hdr"

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

namespace {

const localization::TextRef kFormatNames[] = {
    LOC_TEXT("format.auto", "Auto"), LOC_TEXT("format.sdr", "SDR"),
    LOC_TEXT("format.scrgb", "scRGB"), LOC_TEXT("format.hdr10", "HDR10")};
const localization::TextRef kFormatTips[] = {
    LOC_TEXT("format.auto.tip",
             "Detect the format automatically from the active Pre/Post-DLSS source."),
    LOC_TEXT("format.sdr.tip", "Treat the source as SDR sRGB."),
    LOC_TEXT("format.scrgb.tip",
             "Treat the source as linear scRGB (1.0 = 80 nits)."),
    LOC_TEXT("format.hdr10.tip", "Treat the source as HDR10 PQ (ST.2084).")};

const localization::TextRef kMeteringDensityNames[] = {
    LOC_TEXT("density.full", "Full"), LOC_TEXT("density.quarter", "1/4"),
    LOC_TEXT("density.sixteenth", "1/16"),
    LOC_TEXT("density.sixty_fourth", "1/64")};
const localization::TextRef kMeteringDensityTips[] = {
    LOC_TEXT("density.full.tip",
             "Sample every pixel. Full-density histogram, highest accuracy."),
    LOC_TEXT("density.quarter.tip",
             "Sample every 2nd pixel in each axis (1/4 of the frame). "
             "Small isolated highlights may be missed."),
    LOC_TEXT("density.sixteenth.tip",
             "Sample every 4th pixel in each axis (1/16 of the frame). "
             "Isolated highlights and fine detail may be missed."),
    LOC_TEXT("density.sixty_fourth.tip",
             "Sample every 8th pixel in each axis (1/64 of the frame). "
             "Suitable only for roughly-uniform content.")};

const localization::TextRef kCatAdvanced = LOC_TEXT("cat.advanced", "Advanced");
const localization::TextRef kCatDiagnostics =
    LOC_TEXT("cat.diagnostics", "Diagnostics");

// A Backbuffer pass has no scene-referred source, so there is nothing for HDR
// to meter there.
bool EnabledOnPass(const void*, const ctl::ValueStore& store,
                   const ctl::InstanceKey& key, const std::string&) {
  const int target = store.Read(pass::kSection, key, pass::kTarget).as_int;
  return target != static_cast<int>(InjectionStage::Backbuffer);
}

}  // namespace

}  // namespace hdr

void RegisterHdrSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterHdrSchema");
  using namespace hdr;

  // --- HDR ---------------------------------------------------------------
  ctl::Section& hdr = schema.AddSection(kSection, ctl::SectionKind::Instanced);
  hdr.Title(LOC_TEXT("title", "HDR"));
  hdr.HeaderToggle(kEnable);
  hdr.EnableSectionWhen(&EnabledOnPass);
  hdr.Add<ctl::BoolSetting>(kEnable, LOC_TEXT("enabled", "Enable"), true)
      .Tooltip(LOC_TEXT("enabled.tip",
                        "Meters the HDR image immediately before this pass. Later passes "
                        "measure the image composed by the previous pass unless metering "
                        "reuse is enabled. Backbuffer passes use SDR composition."));
  hdr.Add<ctl::FloatSetting>(kHighlightHeadroom,
                             LOC_TEXT("highlight_headroom", "Highlight Headroom"),
                             1.0f)
      .ClampRange(0.25f, 16.0f)
      .Tooltip(LOC_TEXT("highlight_headroom.tip",
                        "Scales the effective HDR peak used by BT.2446C. This does not "
                        "change how exposure is interpreted."));

  hdr.Add<ctl::EnumSetting>(kFormat, LOC_TEXT("format", "Format"), 0)
      .Items(kFormatNames, kFormatTips, 4)
      .AsGroup()
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("format.tip",
                        "How the source image is encoded. Auto detects from the DLSS "
                        "output's DXGI format."));
  hdr.Add<ctl::IntSetting>(kMeasureInterval,
                           LOC_TEXT("measure_interval", "Measure Interval"), 4)
      .ClampRange(1, 32)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("measure_interval.tip",
                        "How often exposure histograms are measured, in frames."));
  hdr.Add<ctl::EnumSetting>(kMeteringDensity,
                            LOC_TEXT("metering_density", "Metering Density"), 0)
      .Items(kMeteringDensityNames, kMeteringDensityTips, 4)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("metering_density.tip",
                        "How many pixels the HDR histogram samples. Full density "
                        "captures every pixel; coarser densities sample fewer "
                        "pixels at lower GPU cost."));
  hdr.Add<ctl::BoolSetting>(kReusePreviousPassMeasurement,
                            LOC_TEXT("reuse_previous_pass",
                                     "Reuse Previous HDR Pass Metering"),
                            false)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("reuse_previous_pass.tip",
                        "For later passes in the same Pre-DLSS or Post-DLSS chain, reuse "
                        "the previous pass's HDR measurement instead of dispatching a "
                        "new histogram. The first pass still measures the game's source."));
  hdr.Add<ctl::FloatSetting>(kBt2446K1, LOC_TEXT("bt2446_k1", "BT.2446C k1"),
                             0.83802f)
      .ClampRange(0.001f, 2.0f)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("bt2446_k1.tip",
                        "Linear-region slope: Y_SDR = k1 * Y_HDR for content below "
                        "the inflection point. Also the slope the log branch matches "
                        "at the knee."));
  hdr.Add<ctl::FloatSetting>(kBt2446K3, LOC_TEXT("bt2446_k3", "BT.2446C k3"),
                             0.74204f)
      .ClampRange(0.0f, 0.95f)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("bt2446_k3.tip",
                        "Shoulder compression strength. Larger values pull the log "
                        "branch down harder, darkening highlights more. k2 and k4 "
                        "are derived from this so the curve stays smooth at the "
                        "knee."));
  hdr.Add<ctl::FloatSetting>(kBt2446Ip,
                             LOC_TEXT("bt2446_ip", "BT.2446C Inflection"), 94.26f)
      .Range(10.0f, 4000.0f)
      .ClampRange(0.001f, 10000.0f)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("bt2446_ip.tip",
                        "Knee position in the BT.2446C curve domain. Content above "
                        "this point is log-compressed; content below is linear. The "
                        "slider covers the useful range for real HDR content. Values "
                        "outside it can still be typed: 10000 disables compression."));
  hdr.AddHookAfter(kBt2446Ip, "Bt2446Derived", &DrawBt2446Derived)
      .Category(kCatAdvanced);
  hdr.Add<ctl::FloatSetting>(kBt2446Crosstalk,
                             LOC_TEXT("crosstalk", "Crosstalk"), 0.04f)
      .ClampRange(0.0f, 0.33f)
      .Category(kCatAdvanced)
      .Tooltip(LOC_TEXT("crosstalk.tip", "BT.2446C pre-desaturation amount."));
  // Layer collapsibles are rendered by the HdrLayers hook for this pass key.
  // Category(nullptr) closes Advanced so the layers remain top-level rows.
  hdr.AddHookAfter(kBt2446Crosstalk, "HdrLayers", &DrawHdrLayers)
      .Category(nullptr);

  hdr.Add<ctl::BoolSetting>(kDetailedStatistics,
                            LOC_TEXT("detailed_statistics",
                                     "Detailed HDR Statistics"),
                            false)
      .Category(kCatDiagnostics)
      .Tooltip(LOC_TEXT("detailed_statistics.tip",
                        "Collect full-frame gain-map range and excursion statistics. Adds two GPU dispatches when enabled."));
  hdr.Add<ctl::FloatSetting>(kGainMinLog2,
                             LOC_TEXT("gain_min", "Gain Min"), -4.0f)
      .ClampRange(-32.0f, 0.0f)
      .Category(kCatDiagnostics)
      .Tooltip(LOC_TEXT("gain_min.tip",
                        "Minimum expected carrier-to-HDR gain in log2 stops. Used only by the diagnostic gain-map statistics and debug views; does not affect composition."));
  hdr.Add<ctl::FloatSetting>(kGainMaxLog2,
                             LOC_TEXT("gain_max", "Gain Max"), 6.0f)
      .ClampRange(0.0f, 32.0f)
      .Category(kCatDiagnostics)
      .Tooltip(LOC_TEXT("gain_max.tip",
                        "Maximum expected carrier-to-HDR gain in log2 stops. Used only by the diagnostic gain-map statistics and debug views; does not affect composition."));
  hdr.AddHookAfter(kGainMaxLog2, "HdrDiagnostics", &DrawHdrDiagnostics)
      .Category(kCatDiagnostics);

  // --- HDR Game Exposure -------------------------------------------------
  ctl::Section& game =
      schema.AddSection(kGameSection, ctl::SectionKind::Instanced);
  game.HeaderToggle(kGameEnabled);
  game.Add<ctl::BoolSetting>(kGameEnabled,
                             LOC_TEXT("game.enabled", "Game Exposure"), true)
      .Tooltip(LOC_TEXT("game.enabled.tip",
                        "Use the game's NGX ExposureTexture as the base exposure. "
                        "If the game does not provide one, this layer falls through "
                        "and Adaptive Exposure can supply the base instead."));
  game.Add<ctl::FloatSetting>(kGameBiasStops,
                              LOC_TEXT("game.bias", "Exposure Bias"), 0.0f)
      .ClampRange(-4.0f, 4.0f)
      .Tooltip(LOC_TEXT("game.bias.tip",
                        "Manual bias relative to the game's exposure, in stops. Zero "
                        "leaves the game's value untouched."))
      .EnableWhen(kGameEnabled);

  // --- HDR Adaptive Exposure ---------------------------------------------
  ctl::Section& adaptive =
      schema.AddSection(kAdaptiveSection, ctl::SectionKind::Instanced);
  adaptive.HeaderToggle(kAdaptiveEnabled);
  adaptive.Add<ctl::BoolSetting>(kAdaptiveEnabled,
                                 LOC_TEXT("adaptive.enabled", "Adaptive Exposure"),
                                 false)
      .Tooltip(LOC_TEXT("adaptive.enabled.tip",
                        "With Game Exposure off, provides global exposure from a "
                        "histogram measurement. With Game Exposure on, applies "
                        "Strength Over Game as a bounded correction (no effect at "
                        "0). Has no effect until the first measurement arrives."));
  adaptive.Add<ctl::FloatSetting>(kAdaptiveMidtone,
                                  LOC_TEXT("adaptive.midtone", "Midtone Target"),
                                  0.18f)
      .ClampRange(0.01f, 0.5f)
      .Tooltip(LOC_TEXT("adaptive.midtone.tip",
                        "Target luminance for the selected population."))
      .EnableWhen(kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedUp,
                                  LOC_TEXT("adaptive.speed_up", "Brighten Speed"),
                                  3.0f)
      .ClampRange(0.0f, 20.0f)
      .Tooltip(LOC_TEXT("adaptive.speed_up.tip",
                        "Maximum exposure increase in stops per second."))
      .EnableWhen(kAdaptiveEnabled);
  adaptive.Add<ctl::FloatSetting>(kAdaptiveSpeedDown,
                                  LOC_TEXT("adaptive.speed_down", "Darken Speed"),
                                  1.0f)
      .ClampRange(0.0f, 20.0f)
      .Tooltip(LOC_TEXT("adaptive.speed_down.tip",
                        "Maximum exposure decrease in stops per second."))
      .EnableWhen(kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveStrengthOverGame,
                              LOC_TEXT("adaptive.strength_over_game",
                                       "Strength Over Game"),
                              0.0f)
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("adaptive.strength_over_game.tip",
                        "When Game Exposure is active, add this fraction of the "
                        "adaptive target's stop correction. With Game disabled, "
                        "Adaptive is the base exposure."))
      .EnableWhen(kAdaptiveEnabled);
  adaptive
      .Add<ctl::FloatSetting>(kAdaptiveMaxCorrection,
                              LOC_TEXT("adaptive.max_correction",
                                       "Max Correction Stops"),
                              0.75f)
      .ClampRange(0.0f, 3.0f)
      .Tooltip(LOC_TEXT("adaptive.max_correction.tip",
                        "Maximum adjustment around Game Exposure when both layers are "
                        "enabled."))
      .EnableWhen(kAdaptiveEnabled);
  hdr_filter_rows::Declare(adaptive, LOC_TEXT("cat.filters", "Filters"));
}

}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

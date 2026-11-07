// The colour-grade rows. See rows.h for why this is a fragment, not a
// feature.

#include "rows.h"
#include "profiling/profile.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "grade/settings.h"

#define LOC_NAMESPACE "grade"

namespace reshade_dlssnr {
namespace grade_rows {

namespace {

const localization::TextRef kLumaModeNames[] = {
    LOC_TEXT("luma_mode.exposure", "Exposure"),
    LOC_TEXT("luma_mode.gain", "Gain"),
    LOC_TEXT("luma_mode.gamma", "Gamma"),
    LOC_TEXT("luma_mode.soft", "Soft")};
const localization::TextRef kLumaModeTips[] = {
    LOC_TEXT("luma_mode.exposure.tip",
             "Multiply linear-light RGB by 2^amount (stops); gamut mapping protects colors "
             "when the result exceeds the output range."),
    LOC_TEXT("luma_mode.gain.tip",
             "Scales OKLab lightness by a bounded multiplicative curve."),
    LOC_TEXT("luma_mode.gamma.tip",
             "Apply a signed power curve to OKLab lightness. Neutral black and white "
             "remain anchored, concentrating the effect in the midtones."),
    LOC_TEXT("luma_mode.soft.tip",
             "A smooth lightness adjustment that fades to zero near black and white.")};

const localization::TextRef kChromaModeNames[] = {
    LOC_TEXT("chroma_mode.vibrance", "Vibrance"),
    LOC_TEXT("chroma_mode.saturation", "Saturation")};
const localization::TextRef kChromaModeTips[] = {
    LOC_TEXT("chroma_mode.vibrance.tip",
             "Boost muted colors more strongly in OKLab, reducing the effect as chroma rises."),
    LOC_TEXT("chroma_mode.saturation.tip",
             "Scale OKLab chroma by the same factor while preserving perceptual hue.")};

// Setting names are built from the caller's prefix, so two instances on one
// section cannot collide.
std::string Named(const std::string& prefix, const char* name) {
  DLSSNR_PROFILE_CTL_L3("Named");
  return prefix + CStrToString(name);
}

}  // namespace

void Declare(ctl::Section& section, const std::string& prefix,
             const char* enabled_setting_name,
             const localization::TextRef& category) {
  Declare(section, prefix, &ctl::BoolFromSetting, enabled_setting_name,
          category);
}

void Declare(ctl::Section& section, const std::string& prefix,
             ctl::EnablePredicate enabled, const void* context,
             const localization::TextRef& category) {
  DLSSNR_PROFILE_CTL_L3("Declare");
  auto named = [&](const char* name) { return Named(prefix, name); };
  // Every row goes through `group`, so the caller's gate and category are
  // applied here and the caller never has to name a row.
  auto group = [&](auto& setting) -> auto& {
    if (enabled != nullptr) setting.EnableWhen(enabled, context);
    setting.Category(category);
    return setting;
  };

  group(section.Add<ctl::FloatSetting>(named(names::kBlackLevel).c_str(),
                                       LOC_TEXT("black_level", "Black Level"),
                                       0.0f))
      .ClampRange(kGradeBlackLevelMin, kGradeBlackLevelMax)
      .Tooltip(LOC_TEXT("black_level.tip",
                        "The OKLab lightness value mapped to black. Positive values darken "
                        "only the deepest shadows; negative values lift the toe."));

  group(section.Add<ctl::FloatSetting>(named(names::kWhiteLevel).c_str(),
                                       LOC_TEXT("white_level", "White Level"),
                                       1.0f))
      .ClampRange(kGradeWhiteLevelMin, kGradeWhiteLevelMax)
      .Tooltip(LOC_TEXT("white_level.tip",
                        "The OKLab lightness value mapped to white. Raising the point lowers "
                        "the image; out-of-gamut colors are compressed instead of channel-clipped."));

  group(section.Add<ctl::FloatSetting>(named(names::kContrast).c_str(),
                                       LOC_TEXT("contrast", "Contrast"), 0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip(LOC_TEXT("contrast.tip",
                        "A pivoted S-curve around middle lightness. Positive increases contrast "
                        "and negative reduces it while keeping black and white anchored."));

  group(section.Add<ctl::FloatSetting>(named(names::kShadows).c_str(),
                                       LOC_TEXT("shadows", "Shadows"), 0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip(LOC_TEXT("shadows.tip",
                        "Adjusts shadows with a smooth curve that fades out before white."));

  group(section.Add<ctl::FloatSetting>(named(names::kHighlights).c_str(),
                                       LOC_TEXT("highlights", "Highlights"), 0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip(LOC_TEXT("highlights.tip",
                        "Adjusts highlights with a smooth curve that fades out before black."));

  group(section.Add<ctl::EnumSetting>(named(names::kLumaMode).c_str(),
                                      LOC_TEXT("brightness_mode", "Brightness Mode"),
                                      0))
      .Items(kLumaModeNames, kLumaModeTips, 4)
      .Tooltip(LOC_TEXT("brightness_mode.tip",
                        "Which algorithm brightness uses. These are alternatives, "
                        "not layers."));

  group(section.Add<ctl::FloatSetting>(named(names::kLuma).c_str(),
                                       LOC_TEXT("brightness", "Brightness"), 0.0f))
      .ClampRange(-2.0f, 2.0f)
      .Tooltip(LOC_TEXT("brightness.tip",
                        "In Exposure mode, the value is in stops (-1 halves linear light; +1 doubles it). Other modes use their own brightness curve."));

  group(section.Add<ctl::EnumSetting>(named(names::kChromaMode).c_str(),
                                      LOC_TEXT("chroma_mode", "Chroma Mode"), 0))
      .Items(kChromaModeNames, kChromaModeTips, 2)
      .Tooltip(LOC_TEXT("chroma_mode.tip",
                        "Which algorithm saturation uses. These are alternatives, "
                        "not layers."));

  group(section.Add<ctl::FloatSetting>(named(names::kChroma).c_str(),
                                       LOC_TEXT("saturation", "Saturation"), 0.0f))
      .ClampRange(-1.0f, 1.5f)
      .Tooltip(LOC_TEXT("saturation.tip",
                        "Adjusts perceptual chroma in OKLab. Out-of-gamut colors are compressed "
                        "toward the neutral axis to avoid channel-clipping artifacts."));

  group(section.Add<ctl::BoolSetting>(named(names::kEdgeSofteningEnabled).c_str(),
                                      LOC_TEXT("edge_softening", "Edge-Aware Softening"),
                                      false))
      .Tooltip(LOC_TEXT("edge_softening.tip",
                        "Blend a joint-bilateral 3x3 neighborhood into the grade input. "
                        "Luminance similarity reduces blur across strong edges."));

  group(section.Add<ctl::FloatSetting>(named(names::kEdgeSoftening).c_str(),
                                       LOC_TEXT("softening_strength", "Softening Strength"),
                                       0.5f))
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("softening_strength.tip",
                        "Amount of edge-aware 3x3 softening."));

  group(section.Add<ctl::BoolSetting>(named(names::kCasEnabled).c_str(),
                                      LOC_TEXT("cas", "CAS Sharpening"), false))
      .Tooltip(LOC_TEXT("cas.tip",
                        "Enable AMD FidelityFX Contrast Adaptive Sharpening on the graded "
                        "result. Runs as a separate dispatch in linear SDR only; skipped for HDR formats."));

  group(section.Add<ctl::FloatSetting>(named(names::kCasSharpness).c_str(),
                                       LOC_TEXT("cas_strength", "CAS Strength"), 0.5f))
      .ClampRange(0.0f, 1.0f)
      .Tooltip(LOC_TEXT("cas_strength.tip",
                        "Sharpening amount for AMD FidelityFX CAS. 0 applies no sharpening, "
                        "1 applies maximum sharpening."));
}

}  // namespace grade_rows
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

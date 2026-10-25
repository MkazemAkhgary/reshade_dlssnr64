// The colour-grade rows. See rows.h for why this is a fragment, not a
// feature.

#include "rows.h"
#include "profiling/profile.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "grade/settings.h"

namespace reshade_dlssnr {
namespace grade_rows {

namespace {

const char* const kLumaModeNames[] = {"Exposure", "Gain", "Gamma", "Soft"};
const char* const kLumaModeTips[] = {
    "Multiply linear-light RGB by 2^amount (stops); gamut mapping protects colors "
    "when the result exceeds the output range.",
    "Scales OKLab lightness by a bounded multiplicative curve.",
    "Apply a signed power curve to OKLab lightness. Neutral black and white "
    "remain anchored, concentrating the effect in the midtones.",
    "A smooth lightness adjustment that fades to zero near black and white."};

const char* const kChromaModeNames[] = {"Saturation", "Vibrance"};
const char* const kChromaModeTips[] = {
    "Scale OKLab chroma by the same factor while preserving perceptual hue.",
    "Boost muted colors more strongly in OKLab, reducing the effect as chroma rises."};

// Setting names are built from the caller's prefix, so two instances on one
// section cannot collide.
std::string Named(const std::string& prefix, const char* name) {
  DLSSNR_PROFILE_CTL_L3("Named");
  return prefix + CStrToString(name);
}

}  // namespace

void Declare(ctl::Section& section, const std::string& prefix,
             ctl::EnablePredicate enabled, const void* context,
             const char* category) {
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
                                       "Black Level", 0.0f))
      .ClampRange(kGradeBlackLevelMin, kGradeBlackLevelMax)
      .Tooltip("The OKLab lightness value mapped to black. Positive values darken "
               "only the deepest shadows; negative values lift the toe.");

  group(section.Add<ctl::FloatSetting>(named(names::kWhiteLevel).c_str(),
                                       "White Level", 1.0f))
      .ClampRange(kGradeWhiteLevelMin, kGradeWhiteLevelMax)
      .Tooltip("The OKLab lightness value mapped to white. Raising the point lowers "
               "the image; out-of-gamut colors are compressed instead of channel-clipped.");

  group(section.Add<ctl::FloatSetting>(named(names::kContrast).c_str(), "Contrast",
                                       0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("A pivoted S-curve around middle lightness. Positive increases contrast "
               "and negative reduces it while keeping black and white anchored.");

  group(section.Add<ctl::FloatSetting>(named(names::kShadows).c_str(), "Shadows",
                                       0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("Adjusts shadows with a smooth curve that fades out before white.");

  group(section.Add<ctl::FloatSetting>(named(names::kHighlights).c_str(),
                                       "Highlights", 0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("Adjusts highlights with a smooth curve that fades out before black.");

  group(section.Add<ctl::EnumSetting>(named(names::kLumaMode).c_str(), "Brightness Mode",
                                      0))
      .Items(kLumaModeNames, kLumaModeTips, 4)
      .Tooltip("Which algorithm brightness uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kLuma).c_str(), "Brightness",
                                       0.0f))
      .ClampRange(-2.0f, 2.0f)
      .Tooltip("In Exposure mode, the value is in stops (-1 halves linear light; +1 doubles it). Other modes use their own brightness curve.");

  group(section.Add<ctl::EnumSetting>(named(names::kChromaMode).c_str(),
                                      "Chroma Mode", 0))
      .Items(kChromaModeNames, kChromaModeTips, 2)
      .Tooltip("Which algorithm saturation uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kChroma).c_str(), "Saturation",
                                       0.0f))
      .ClampRange(-1.0f, 1.5f)
      .Tooltip("Adjusts perceptual chroma in OKLab. Out-of-gamut colors are compressed "
               "toward the neutral axis to avoid channel-clipping artifacts.");

  group(section.Add<ctl::BoolSetting>(named(names::kEdgeSofteningEnabled).c_str(),
                                      "Edge-Aware Softening", false))
      .Tooltip("Blend a joint-bilateral 3x3 neighborhood into the grade input. "
               "Luminance similarity reduces blur across strong edges.");

  group(section.Add<ctl::FloatSetting>(named(names::kEdgeSoftening).c_str(),
                                       "Softening Strength", 0.5f))
      .ClampRange(0.0f, 1.0f)
      .Tooltip("Amount of edge-aware 3x3 softening.");

  group(section.Add<ctl::BoolSetting>(named(names::kCasEnabled).c_str(),
                                      "CAS Sharpening", false))
      .Tooltip("Apply AMD FidelityFX Contrast Adaptive Sharpening after the "
               "grade. CAS operates in linear SDR only and is skipped for HDR formats.");

  group(section.Add<ctl::FloatSetting>(named(names::kCasSharpness).c_str(),
                                       "CAS Strength", 0.5f))
      .ClampRange(0.0f, 1.0f)
      .Tooltip("FidelityFX CAS sharpness (0 is least, 1 is most).");
}

}  // namespace grade_rows
}  // namespace reshade_dlssnr

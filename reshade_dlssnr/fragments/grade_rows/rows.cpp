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

const char* const kLumaModeNames[] = {"Gain", "Gamma", "Exposure", "Soft"};
const char* const kLumaModeTips[] = {
    "Multiply luminance by 2^amount. An equal push either way.",
    "Raise luminance to a power. Black stays black and white stays white, "
    "so the effect concentrates in the midtones.",
    "Multiply linear RGB by 2^amount (stops), preserving chromaticity until clipping.",
    "A parabolic lift, zero at both ends and strongest at mid grey."};

const char* const kChromaModeNames[] = {"Saturation", "Vibrance"};
const char* const kChromaModeTips[] = {
    "Blend toward luma, so every pixel moves by the same factor.",
    "Smart saturation: strongly boost muted colours while progressively "
    "reducing the effect on colours that are already saturated."};

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
      .Tooltip("The linear value mapped to black. Raising crushes the "
               "shadows; lowering lifts black.");

  group(section.Add<ctl::FloatSetting>(named(names::kWhiteLevel).c_str(),
                                       "White Level", 1.0f))
      .ClampRange(kGradeWhiteLevelMin, kGradeWhiteLevelMax)
      .Tooltip("The linear value mapped to white.");

  group(section.Add<ctl::FloatSetting>(named(names::kContrast).c_str(), "Contrast",
                                       0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("Stretches the image around mid grey. Positive steepens, "
               "negative flattens.");

  group(section.Add<ctl::FloatSetting>(named(names::kShadows).c_str(), "Shadows",
                                       0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("Scales the dark end. Fades out before it reaches white.");

  group(section.Add<ctl::FloatSetting>(named(names::kHighlights).c_str(),
                                       "Highlights", 0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("Scales the bright end. Fades out before it reaches black.");

  group(section.Add<ctl::EnumSetting>(named(names::kLumaMode).c_str(), "Brightness Mode",
                                      0))
      .Items(kLumaModeNames, kLumaModeTips, 4)
      .Tooltip("Which algorithm brightness uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kLuma).c_str(), "Brightness",
                                       0.0f))
      .ClampRange(-1.0f, 1.0f)
      .Tooltip("In Exposure mode, the value is in stops (-1 halves linear light; +1 doubles it). Other modes use their own brightness curve.");

  group(section.Add<ctl::EnumSetting>(named(names::kChromaMode).c_str(),
                                      "Chroma Mode", 0))
      .Items(kChromaModeNames, kChromaModeTips, 2)
      .Tooltip("Which algorithm saturation uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kChroma).c_str(), "Saturation",
                                       0.0f))
      .ClampRange(-0.95f, 1.0f)
      .Tooltip("Negative desaturates, positive saturates.");

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

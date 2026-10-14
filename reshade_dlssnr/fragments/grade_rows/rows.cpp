// The nine colour-grade rows. See rows.h for why this is a fragment, not a
// feature.

#include "rows.h"

#include "ctl/builtin_settings.h"
#include "ctl/schema.h"
#include "grade/settings.h"

namespace reshade_dlssnr {
namespace grade_rows {

namespace {

const char* const kLumaModeNames[] = {"Gain", "Gamma", "Lift", "Soft"};
const char* const kLumaModeTips[] = {
    "Multiply luminance by 2^amount. An equal push either way.",
    "Raise luminance to a power. Black stays black and white stays white, "
    "so the effect concentrates in the midtones.",
    "Raise the black level. Positive moves a pixel up by the room above it, "
    "negative down by the room below it.",
    "A parabolic lift, zero at both ends and strongest at mid grey."};

const char* const kChromaModeNames[] = {"Saturation", "Vibrance"};
const char* const kChromaModeTips[] = {
    "Blend toward luma, so every pixel moves by the same factor.",
    "Smart saturation: strongly boost muted colours while progressively "
    "reducing the effect on colours that are already saturated."};

// Setting names are built from the caller's prefix, so two instances on one
// section cannot collide.
std::string Named(const std::string& prefix, const char* name) {
  return prefix + CStrToString(name);
}

}  // namespace

void Declare(ctl::Section& section, const std::string& prefix,
             ctl::EnablePredicate enabled, const void* context,
             const char* category) {
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
      .Range(kGradeBlackLevelMin, kGradeBlackLevelMax)
      .Tooltip("The linear value mapped to black. Raising crushes the "
               "shadows; lowering lifts black.");

  group(section.Add<ctl::FloatSetting>(named(names::kWhiteLevel).c_str(),
                                       "White Level", 1.0f))
      .Range(kGradeWhiteLevelMin, kGradeWhiteLevelMax)
      .Tooltip("The linear value mapped to white.");

  group(section.Add<ctl::FloatSetting>(named(names::kContrast).c_str(), "Contrast",
                                       0.0f))
      .Range(-1.0f, 1.0f)
      .Tooltip("Stretches the image around mid grey. Positive steepens, "
               "negative flattens.");

  group(section.Add<ctl::FloatSetting>(named(names::kShadows).c_str(), "Shadows",
                                       0.0f))
      .Range(-1.0f, 1.0f)
      .Tooltip("Scales the dark end. Fades out before it reaches white.");

  group(section.Add<ctl::FloatSetting>(named(names::kHighlights).c_str(),
                                       "Highlights", 0.0f))
      .Range(-1.0f, 1.0f)
      .Tooltip("Scales the bright end. Fades out before it reaches black.");

  group(section.Add<ctl::EnumSetting>(named(names::kLumaMode).c_str(), "Luma Mode",
                                      0))
      .Items(kLumaModeNames, kLumaModeTips, 4)
      .Tooltip("Which algorithm brightness uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kLuma).c_str(), "Brightness",
                                       0.0f))
      .Range(-1.0f, 1.0f)
      .Tooltip("Negative darkens, positive brightens.");

  group(section.Add<ctl::EnumSetting>(named(names::kChromaMode).c_str(),
                                      "Chroma Mode", 0))
      .Items(kChromaModeNames, kChromaModeTips, 2)
      .Tooltip("Which algorithm saturation uses. These are alternatives, "
               "not layers.");

  group(section.Add<ctl::FloatSetting>(named(names::kChroma).c_str(), "Saturation",
                                       0.0f))
      .Range(-0.95f, 1.0f)
      .Tooltip("Negative desaturates, positive saturates.");
}

}  // namespace grade_rows
}  // namespace reshade_dlssnr

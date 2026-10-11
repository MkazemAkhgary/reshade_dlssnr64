#include <imgui.h>
#include <reshade.hpp>

#include "grade/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

namespace {

const char* const kLumaModeItems[] = {"Gain", "Gamma", "Lift", "Soft"};
constexpr int kLumaModeCount = 4;
const char* const kLumaModeTips[] = {
    "Multiply luminance by 2^amount. An equal push either way.",
    "Raise luminance to a power. Black stays black and white stays white, "
    "so the effect concentrates in the midtones.",
    "Raise the black level. Positive moves a pixel up by the room above it, "
    "negative down by the room below it.",
    "A parabolic lift, zero at both ends and strongest at mid grey."};

const char* const kChromaModeItems[] = {"Saturation", "Vibrance"};
constexpr int kChromaModeCount = 2;
const char* const kChromaModeTips[] = {
    "Blend toward luma, so every pixel moves by the same factor.",
    "Smart saturation: strongly boost muted colours while progressively "
    "reducing the effect on colours that are already saturated."};

}  // namespace

void DrawGradeSection(const char* id,
                      const char* label,
                      const char* tooltip,
                      GradeSettings& grade,
                      bool& changed) {
  ImGui::PushID(id);

  changed |= DrawCheckboxAtLabel("enabled", &grade.enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip != nullptr) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine();
  ImGui::TextUnformatted(label);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && tooltip != nullptr) {
    Tip("%s", tooltip);
  }

  WithDisabled(!grade.enabled, [&] {
    changed |= DrawFloatRow(0, "black_level", "Black Level",
                            "The linear value mapped to black. Raising "
                            "crushes the shadows; lowering lifts black.",
                            &grade.black_level, nullptr, 0.0f,
                            kGradeBlackLevelMin, kGradeBlackLevelMax, 0.0f);

    changed |= DrawFloatRow(0, "white_level", "White Level",
                            "The linear value mapped to white.",
                            &grade.white_level, nullptr, 1.0f,
                            kGradeWhiteLevelMin, kGradeWhiteLevelMax, 1.0f);

    changed |= DrawFloatRow(0, "contrast", "Contrast",
                            "Stretches the image around mid grey. Positive "
                            "steepens, negative flattens.",
                            &grade.contrast, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    changed |= DrawFloatRow(0, "shadows", "Shadows",
                            "Scales the dark end. Fades out before it "
                            "reaches white.",
                            &grade.shadows, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    changed |= DrawFloatRow(0, "highlights", "Highlights",
                            "Scales the bright end. Fades out before it "
                            "reaches black.",
                            &grade.highlights, nullptr, 0.0f, -1.0f, 1.0f,
                            0.0f);

    int luma_mode = static_cast<int>(grade.luma_mode);
    if (DrawComboRow("luma_mode", "Luma Mode",
                     "Which algorithm brightness uses. These are "
                     "alternatives, not layers.",
                     &luma_mode, kLumaModeItems, kLumaModeTips,
                     kLumaModeCount)) {
      grade.luma_mode = static_cast<GradeLumaMode>(luma_mode);
      changed = true;
    }

    changed |= DrawFloatRow(0, "luma", "Brightness",
                            "Negative darkens, positive brightens.",
                            &grade.luma, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    int chroma_mode = static_cast<int>(grade.chroma_mode);
    if (DrawComboRow("chroma_mode", "Chroma Mode",
                     "Which algorithm saturation uses. These are "
                     "alternatives, not layers.",
                     &chroma_mode, kChromaModeItems, kChromaModeTips,
                     kChromaModeCount)) {
      grade.chroma_mode = static_cast<GradeChromaMode>(chroma_mode);
      changed = true;
    }

    changed |= DrawFloatRow(0, "chroma", "Saturation",
                            "Negative desaturates, positive saturates.",
                            &grade.chroma, nullptr, 0.0f, -0.95f, 1.0f, 0.0f);
  });

  ImGui::PopID();
}

}  // namespace reshade_dlssnr

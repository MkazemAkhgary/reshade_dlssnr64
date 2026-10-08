// Grade settings: modes, per-grade values, and level ranges.
#pragma once

#include <string>

namespace reshade_dlssnr {

// ---------------------------------------------------------------------------
// Colour grade
// ---------------------------------------------------------------------------
//
// A grade the user applies at three points: before the model reads, after
// it writes, and on the final backbuffer. One luma mode and one chroma mode
// are active at a time; stacking two would make them fight.

enum class GradeLumaMode : int {
  Gain = 0,   // x2^amount; symmetric, clips at white
  Gamma = 1,  // power on normalized luma; midtone-weighted, never clips
  Lift = 2,   // offset scaled by headroom; moves shadows
  Soft = 3,   // parabolic lift; strongest at mid grey
};

enum class GradeChromaMode : int {
  Saturation = 0,  // blend toward luma; uniform
  Vibrance = 1,    // smart saturation; muted colors move more
};

struct GradeSettings {
  bool enabled = false;
  float luma = 0.0f;
  float chroma = 0.0f;
  GradeLumaMode luma_mode = GradeLumaMode::Gamma;
  GradeChromaMode chroma_mode = GradeChromaMode::Saturation;
  float black_level = 0.0f;
  float white_level = 1.0f;
  float contrast = 0.0f;
  float shadows = 0.0f;
  float highlights = 0.0f;
};

constexpr float kGradeBlackLevelMin = -0.25f;
constexpr float kGradeBlackLevelMax =  0.20f;

constexpr float kGradeWhiteLevelMin =  0.25f;
constexpr float kGradeWhiteLevelMax =  4.0f;

void LoadGrade(const std::string& prefix, GradeSettings& grade);
void SaveGrade(const std::string& prefix, const GradeSettings& grade);

}  // namespace reshade_dlssnr

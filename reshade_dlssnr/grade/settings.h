// Grade settings: modes, per-grade values, and level ranges.
#pragma once

namespace reshade_dlssnr {

// ---------------------------------------------------------------------------
// Colour grade
// ---------------------------------------------------------------------------
//
// A grade the user applies at three points: before the model reads, after
// it writes, and on the final backbuffer. One luma mode and one chroma mode
// are active at a time; stacking two would make them fight.

enum class GradeLumaMode : int {
  Gain = 0,      // Bounded multiplicative curve on OKLab lightness
  Gamma = 1,     // Power curve on OKLab lightness; endpoints stay anchored
  Exposure = 2,  // x2^amount on linear RGB; preserves chromaticity until clipping
  Soft = 3,      // parabolic lift; strongest at mid grey
};

enum class GradeChromaMode : int {
  Saturation = 0,  // uniform perceptual chroma scale
  Vibrance = 1,    // muted colors receive more chroma
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
  bool edge_softening_enabled = false;
  float edge_softening = 0.5f;
  bool cas_enabled = false;
  float cas_sharpness = 0.5f;
};

// Bounds chosen so the levels curve stays useful: past ~0.2 the black point
// eats the whole shadow range, and below ~0.7 the white point has nothing left
// to compress.
constexpr float kGradeBlackLevelMin = -0.40f;
constexpr float kGradeBlackLevelMax =  0.20f;

constexpr float kGradeWhiteLevelMin =  0.70f;
constexpr float kGradeWhiteLevelMax =  2.00f;

}  // namespace reshade_dlssnr

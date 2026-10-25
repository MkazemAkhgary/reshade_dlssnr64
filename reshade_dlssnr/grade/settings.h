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
  Exposure = 0,  // x2^amount on linear RGB; preserves chromaticity until clipping
  Gain = 1,      // Bounded multiplicative curve on OKLab lightness
  Gamma = 2,     // Power curve on OKLab lightness; endpoints stay anchored
  Soft = 3,      // parabolic lift; strongest at mid grey
};

enum class GradeChromaMode : int {
  Vibrance = 0,    // muted colors receive more chroma
  Saturation = 1,  // uniform perceptual chroma scale
};

struct GradeSettings {
  bool enabled = false;
  float luma = 0.0f;
  float chroma = 0.0f;
  GradeLumaMode luma_mode = GradeLumaMode::Exposure;
  GradeChromaMode chroma_mode = GradeChromaMode::Vibrance;
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

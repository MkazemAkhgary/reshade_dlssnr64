#include "settings/grade.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

namespace {

float ClampRange(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

float ClampGradeAmount(float v) {
  return ClampRange(v, -1.0f, 1.0f);
}

}  // namespace

void LoadGrade(const char* prefix, GradeSettings& grade) {
  const std::string p = prefix;
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Enabled").c_str(), grade.enabled);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR", (p + "Luma").c_str(),
                            grade.luma);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Chroma").c_str(), grade.chroma);

  int luma_mode = static_cast<int>(grade.luma_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "LumaMode").c_str(), luma_mode);
  if (luma_mode < 0 || luma_mode > static_cast<int>(GradeLumaMode::Soft)) {
    luma_mode = static_cast<int>(GradeLumaMode::Gamma);
  }
  grade.luma_mode = static_cast<GradeLumaMode>(luma_mode);

  int chroma_mode = static_cast<int>(grade.chroma_mode);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "ChromaMode").c_str(), chroma_mode);
  if (chroma_mode < 0 ||
      chroma_mode > static_cast<int>(GradeChromaMode::ChromaGain)) {
    chroma_mode = static_cast<int>(GradeChromaMode::Saturation);
  }
  grade.chroma_mode = static_cast<GradeChromaMode>(chroma_mode);

  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "BlackPoint").c_str(), grade.black_point);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "WhitePoint").c_str(), grade.white_point);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Contrast").c_str(), grade.contrast);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Shadows").c_str(), grade.shadows);
  reshade::get_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Highlights").c_str(), grade.highlights);

  grade.luma = ClampGradeAmount(grade.luma);
  grade.chroma = ClampGradeAmount(grade.chroma);
  grade.black_point =
      ClampRange(grade.black_point, kGradeBlackPointMin, kGradeBlackPointMax);
  grade.white_point =
      ClampRange(grade.white_point, kGradeWhitePointMin, kGradeWhitePointMax);
  grade.contrast = ClampGradeAmount(grade.contrast);
  grade.shadows = ClampGradeAmount(grade.shadows);
  grade.highlights = ClampGradeAmount(grade.highlights);
}

void SaveGrade(const char* prefix, const GradeSettings& grade) {
  const std::string p = prefix;
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Enabled").c_str(), grade.enabled);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR", (p + "Luma").c_str(),
                            grade.luma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Chroma").c_str(), grade.chroma);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "LumaMode").c_str(),
                            static_cast<int>(grade.luma_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "ChromaMode").c_str(),
                            static_cast<int>(grade.chroma_mode));
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "BlackPoint").c_str(), grade.black_point);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "WhitePoint").c_str(), grade.white_point);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Contrast").c_str(), grade.contrast);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Shadows").c_str(), grade.shadows);
  reshade::set_config_value(nullptr, "RESHADE_DLSSNR",
                            (p + "Highlights").c_str(), grade.highlights);
}

}  // namespace reshade_dlssnr

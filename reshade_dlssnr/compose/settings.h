// Residual edit settings: how the model's edit is carried onto the source.
#pragma once

#include <string>

namespace reshade_dlssnr {

// Which algorithm reconstructs full resolution from the working-scale
// model input and output. EASU is the original FSR 1.0 upsampler;
// joint bilateral uses the full-resolution original as an edge guide.
enum class UpsampleMode : int {
  Easu = 0,
  JointBilateral = 1,
};

struct ResidualSettings {
  float edit_strength = 1.0f;
  float chroma_strength = 1.0f;
  float edit_gamma = 1.0f;
  float highlight_protect = 0.0f;
  float shadow_protect = 0.0f;

  // Which upsampler reconstructs full resolution from the working-scale
  // model input and output. Default is the joint-bilateral path, which
  // preserves detail better at low working scales; EASU is retained for
  // comparison.
  UpsampleMode upsample_mode = UpsampleMode::JointBilateral;

  // Edge-preservation exponent for the joint-bilateral upsample of the
  // model's input and output. Higher values follow the full-resolution
  // image's edges more sharply; lower values blend more across the
  // low-resolution neighborhood. It does not scale the residual itself.
  float upsample_sharpness = 8.0f;

  float ratio_eps = 1e-2f;
  float ratio_brighten = 64.0f;
  float ratio_darken = 1.0f / 16.0f;

};

// Strength section: blends of the model's luma and chroma edits applied
// when the model output is composed back onto the source.
struct StrengthSettings {
  float luma_strength = 1.0f;
  float chroma_strength = 1.0f;
};

// Soft compression of the residual delta before it is blended onto the
// source. Sliders are gated on `enabled`.
struct SoftCompressionSettings {
  bool enabled = false;
  float max_brighten = 0.5f;
  float max_darken = 0.5f;
  float max_chroma = 0.5f;
  float knee = 1.0f;
};

void LoadResidual(const std::string& prefix, ResidualSettings& residual);
void SaveResidual(const std::string& prefix, const ResidualSettings& residual);
void LoadStrength(const std::string& prefix, StrengthSettings& strength);
void SaveStrength(const std::string& prefix, const StrengthSettings& strength);
void LoadSoftCompression(const std::string& prefix,
                         SoftCompressionSettings& soft_compression);
void SaveSoftCompression(const std::string& prefix,
                         const SoftCompressionSettings& soft_compression);

}  // namespace reshade_dlssnr

// Residual edit settings: how the model's edit is carried onto the source.
#pragma once

namespace reshade_dlssnr {

// Which algorithm reconstructs full resolution from the working-scale
// model input and output. EASU is the original FSR 1.0 upsampler;
// joint bilateral uses the full-resolution original as an edge guide.
enum class UpsampleMode : int {
  Easu = 0,
  JointBilateral = 1,
};

// Spatial extent of the joint-bilateral kernel. Larger kernels spread
// the reconstruction across more low-resolution samples, smoothing
// curves and diagonals and reducing sample-grid modulation at very low
// working scales, at proportionally higher texture-load cost. The
// spatial sigma is derived from the kernel half-size (sigma = half) so
// the outer diagonal tap is weighted the same at every size.
enum class UpsampleKernel : int {
  Kernel3x3 = 0,
  Kernel5x5 = 1,
  Kernel7x7 = 2,
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
  // Joint-bilateral kernel size. 3x3 is the default and lowest-cost;
  // 5x5 and 7x7 widen the spatial support. The kernel and the spatial
  // sigma are coupled: sigma = half-size, so the falloff shape is the
  // same across sizes and only the number of contributing taps changes.
  UpsampleKernel upsample_kernel = UpsampleKernel::Kernel3x3;

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

}  // namespace reshade_dlssnr

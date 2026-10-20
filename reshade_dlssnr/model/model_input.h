// Model input preparation. Downsamples the source into the working-scale
// buffer with exact pixel-footprint box/area averaging.
#pragma once

#include <d3d12.h>
#include <cstdint>

#include "command_context.h"
#include "grade/settings.h"
#include "compute_engine.h"
#include "pass_resources.h"
#include "shaders/common/color_constants.h"

namespace reshade_dlssnr::model_pipeline {

// HDR gain-map split/compose uniforms, written to ComputeConstants::hdr for the shared HDR/gain-map block.
// Zero-initialised `format` means SDR sRGB, 1 scRGB, 2 HDR10 PQ.
struct HdrUniforms {
  // Shader format code: 0 = SDR sRGB, 1 = scRGB, 2 = HDR10 PQ.
  float format = 0.0f;
  float exposure = 1.0f;

  // BT.2446 Method C. k2 and k4 are derived in the shader from k1, k3,
  // and the HDR inflection point so the curve is C1-continuous.
  float bt2446_k1 = 0.83802f;
  float bt2446_k3 = 0.74204f;
  float bt2446_ip = 94.26f;
  float bt2446_crosstalk = 0.04f;

  // packed.a stores the direct linear HDR-to-SDR-carrier luminance multiplier.
  // The RGBA32F carrier path intentionally avoids offset-based gain encoding.
  float gain_min_log2 = -4.0f;
  float gain_max_log2 = 6.0f;
  // Absolute effective scene-white peak in nits. Histogram and exposure
  // use the 100-nit SDR/gain-map working domain; scRGB uses 80 nits only
  // when decoding the source format at the domain boundary.
  float effective_scene_white_nits =
      color_constants::kScRgbReferenceWhiteNits;

  // Local-exposure controls are shared by carrier split and inverse map.
  float local_exposure_strength = 0.0f;
  float local_max_brighten = 2.50f;
  float local_max_darken = 2.50f;
  float midtone = 0.18f;
  float local_shadow_strength = 1.0f;
  float local_highlight_strength = 1.0f;
  float local_shadow_range = 1.0f;
  float local_highlight_range = 1.0f;
  float local_highlight_protection = 1.0f;
  float local_shadow_protection = 1.0f;
  float local_detail_retention = 1.0f;
  float local_edge_protection = 0.5f;
  float local_radius_px = 32.0f;
  int local_samples = 12;
  bool restore_local_exposure = false;
  float restore_local_mode = 1.0f;
  bool fp16_unlifted_reference = false;
};

void SetLocalExposureConstants(ComputeConstants& constants,
                              const HdrUniforms& hdr);

// The source is already in the model domain (gain-map split happens in its
// own pipeline step for HDR). This pass only downsamples it to working scale.
bool PrepareInput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* full_input,
    ID3D12Resource** out_model_color,
    ID3D12Resource** out_model_output,
    unsigned int* out_eval_width,
    unsigned int* out_eval_height,
    unsigned int work_width,
    unsigned int work_height,
    bool gain_map_source,
    bool allow_identity = false,
    const GradeSettings* fused_grade = nullptr);

}  // namespace reshade_dlssnr::model_pipeline

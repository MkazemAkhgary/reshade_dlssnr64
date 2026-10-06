// Model input preparation. Downsamples the source into the working-scale
// buffer with exact pixel-footprint box/area averaging.
#pragma once

#include <d3d12.h>

#include "command_context.h"
#include "compute_engine.h"
#include "pass_resources.h"

namespace reshade_dlssnr::model_pipeline {

// HDR gain-map split/compose uniforms, written at kHdrSlotBase in ComputeConstants::params.
// Zero-initialised `format` means SDR sRGB, 1 scRGB, 2 HDR10 PQ.
struct HdrUniforms {
  // Shader format code: 0 = SDR sRGB, 1 = scRGB, 2 = HDR10 PQ.
  float format = 0.0f;
  float exposure = 1.0f;

  // BT.2446 Method C. k2/k4 are derived from k1/k3 when settings are loaded.
  float bt2446_k1 = 0.83802f;
  float bt2446_k2 = 15.09968f;
  float bt2446_k3 = 0.74204f;
  float bt2446_k4 = 78.99439f;
  float bt2446_crosstalk = 0.04f;

  // Raw ISO 21496-1 gain-map apply parameters. Gain itself is stored as raw
  // log2 in packed.a, so no unnormalization/gamma step is needed.
  float gain_base_offset = 1.0f / 64.0f;
  float gain_alternate_offset = 1.0f / 64.0f;
  float gain_min_log2 = -4.0f;
  float gain_max_log2 = 6.0f;
  float effective_scene_white = 1.0f;
};

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
    bool gain_map_source);

}  // namespace reshade_dlssnr::model_pipeline

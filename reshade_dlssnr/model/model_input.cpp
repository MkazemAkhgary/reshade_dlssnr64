#include "model/model_input.h"
#include "profiling/profile.h"

#include "grade/grade.h"

namespace reshade_dlssnr::model_pipeline {

void SetLocalExposureConstants(ComputeConstants& constants,
                               const HdrUniforms& hdr) {
  DLSSNR_PROFILE_RENDER_L3("SetLocalExposureConstants");
  constants.hdr.LocalExposure.x = hdr.local_exposure_strength;
  constants.hdr.LocalExposure.y = hdr.local_max_brighten;
  constants.hdr.LocalExposure.z = hdr.local_max_darken;
  constants.hdr.LocalExposure.w = hdr.midtone;
  constants.hdr.LocalExposureParams.x = hdr.local_radius_px;
  constants.hdr.LocalExposureParams.y = static_cast<float>(hdr.local_samples);
  constants.localExposure.Shape.x = hdr.local_shadow_range;
  constants.localExposure.Shape.y = hdr.local_highlight_range;
  constants.localExposure.Shape.z = hdr.local_highlight_protection;
  constants.localExposure.Shape.w = hdr.local_shadow_protection;
  constants.localExposure.Detail.x = hdr.local_detail_retention;
  constants.localExposure.Detail.y = hdr.local_shadow_strength;
  constants.localExposure.Detail.z = hdr.local_highlight_strength;
  constants.localExposure.Detail.w = hdr.local_edge_protection;
}

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
    bool allow_identity,
    const GradeSettings* fused_grade) {
  DLSSNR_PROFILE_RENDER_L2("PrepareInput");
  if (out_model_color == nullptr || out_model_output == nullptr ||
      codec == nullptr || command_list == nullptr || full_input == nullptr) {
    return false;
  }

  const D3D12_RESOURCE_DESC input_desc = full_input->GetDesc();
  if (allow_identity && !gain_map_source && input_desc.Width == work_width &&
      input_desc.Height == work_height) {
    // At exact 1:1 SDR scale with no input grade, the original is already the
    // required model input. Avoid a full-resolution copy/dispatch and preserve
    // the game resource for source-based composition.
    state.Transition(command_list, full_input,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    *out_model_color = full_input;
    *out_model_output = resources.GetWorkingOutput();
    *out_eval_width = work_width;
    *out_eval_height = work_height;
    return true;
  }

  ComputeConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;
  constants.shader.Params0.w = gain_map_source ? 1.0f : 0.0f;
  if (fused_grade != nullptr && fused_grade->enabled)
    SetGradeConstants(constants, *fused_grade);
  const char* pass_name = "PrepareModelInput";

  state.Transition(command_list, full_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {full_input, full_input, full_input, full_input};
  ID3D12Resource* uavs[kUavCount] = {resources.GetWorkingInput()};

  if (!codec->Dispatch(pass_name, command_list, constants, srvs, uavs)) {
    return false;
  }

  state.Transition(command_list, resources.GetWorkingInput(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  *out_model_color = resources.GetWorkingInput();
  *out_model_output = resources.GetWorkingOutput();
  *out_eval_width = work_width;
  *out_eval_height = work_height;
  return true;
}

}  // namespace reshade_dlssnr::model_pipeline

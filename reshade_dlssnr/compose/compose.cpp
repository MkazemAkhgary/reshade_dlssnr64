#include "compose/compose.h"

#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* packed_source,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    unsigned int full_width,
    unsigned int full_height,
    const float* params,
    const ResidualSettings& residual,
    bool rcas_enabled,
    float rcas_strength,
    const GradeSettings* grade_input,
    const HdrUniforms* hdr) {
  if (codec == nullptr || command_list == nullptr || packed_source == nullptr ||
      low_input == nullptr || low_enhanced == nullptr || params == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    constants.params[kShaderParamSlotBase + i] = params[i];
  }
  constants.params[kResidualSlotBase + 0] = residual.edit_strength;
  constants.params[kResidualSlotBase + 1] = residual.chroma_strength;
  constants.params[kResidualSlotBase + 2] = residual.edit_gamma;
  constants.params[kResidualSlotBase + 3] = residual.highlight_protect;
  constants.params[kResidualSlotBase + 4] = residual.shadow_protect;
  constants.params[kResidualSlotBase + 5] = residual.ratio_eps;
  constants.params[kResidualSlotBase + 6] = residual.ratio_brighten;
  constants.params[kResidualSlotBase + 7] = residual.ratio_darken;

  const char* pass_name = "ComposeModelOutput";
  if (hdr != nullptr) {
    pass_name = "ComposeModelOutputHdr";
    constants.params[kHdrSlotBase + 0] = hdr->format;
    constants.params[kHdrSlotBase + 1] = hdr->exposure;
    constants.params[kHdrSlotBase + 2] = hdr->bt2446_k1;
    constants.params[kHdrSlotBase + 3] = hdr->bt2446_k2;
    constants.params[kHdrSlotBase + 4] = hdr->bt2446_k4;
    constants.params[kHdrSlotBase + 5] = hdr->bt2446_crosstalk;
    constants.params[kHdrSlotBase + 6] = hdr->gain_base_offset;
    constants.params[kHdrSlotBase + 7] = hdr->gain_alternate_offset;
    constants.params[kHdrSlotBase + 8] = hdr->gain_min_log2;
    constants.params[kHdrSlotBase + 9] = hdr->gain_max_log2;
    constants.params[kHdrSlotBase + 10] = hdr->effective_scene_white;
    constants.params[kHdrSlotBase + 11] = 0.0f;

    // FsrEasuCon with inputViewport == inputSize: the working-scale textures
    // are sampled 1:1, so viewport and texture size coincide. con3 is not
    // stored; the compose derives it from con1.y.
    const D3D12_RESOURCE_DESC low_desc = low_enhanced->GetDesc();
    const float in_w = static_cast<float>(low_desc.Width);
    const float in_h = static_cast<float>(low_desc.Height);
    const float out_w = static_cast<float>(full_width);
    const float out_h = static_cast<float>(full_height);

    constants.params[kEasuSlotBase + 0] = in_w / out_w;
    constants.params[kEasuSlotBase + 1] = in_h / out_h;
    constants.params[kEasuSlotBase + 2] = 0.5f * in_w / out_w - 0.5f;
    constants.params[kEasuSlotBase + 3] = 0.5f * in_h / out_h - 0.5f;
    constants.params[kEasuSlotBase + 4] = 1.0f / in_w;
    constants.params[kEasuSlotBase + 5] = 1.0f / in_h;
    constants.params[kEasuSlotBase + 6] = 1.0f / in_w;
    constants.params[kEasuSlotBase + 7] = -1.0f / in_h;
    constants.params[kEasuSlotBase + 8] = -1.0f / in_w;
    constants.params[kEasuSlotBase + 9] = 2.0f / in_h;
    constants.params[kEasuSlotBase + 10] = 1.0f / in_w;
    constants.params[kEasuSlotBase + 11] = 2.0f / in_h;

    constants.params[kRcasSlotBase + 0] = rcas_enabled ? 1.0f : 0.0f;
    constants.params[kRcasSlotBase + 1] = rcas_strength;

    if (grade_input != nullptr) {
      constants.params[kGradeSlotBase + 0] = grade_input->enabled ? 1.0f : 0.0f;
      constants.params[kGradeSlotBase + 1] = grade_input->luma;
      constants.params[kGradeSlotBase + 2] = grade_input->chroma;
      constants.params[kGradeSlotBase + 3] =
          static_cast<float>(static_cast<int>(grade_input->luma_mode));
      constants.params[kGradeSlotBase + 4] =
          static_cast<float>(static_cast<int>(grade_input->chroma_mode));
      constants.params[kGradeSlotBase + 5] = grade_input->black_level;
      constants.params[kGradeSlotBase + 6] = grade_input->white_level;
      constants.params[kGradeSlotBase + 7] = grade_input->contrast;
      constants.params[kGradeSlotBase + 8] = grade_input->shadows;
      constants.params[kGradeSlotBase + 9] = grade_input->highlights;
    }
  }

  state.Transition(command_list, packed_source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {packed_source, low_input, low_enhanced};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(pass_name, command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

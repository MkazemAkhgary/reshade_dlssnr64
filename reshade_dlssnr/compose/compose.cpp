#include "compose/compose.h"

#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* packed_source,
    ID3D12Resource* hdr_source,
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
      hdr_source == nullptr || low_input == nullptr || low_enhanced == nullptr ||
      params == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  float* shader_params = &constants.shader.Params0.x;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    shader_params[i] = params[i];
  }
  constants.residual.EditStrength = residual.edit_strength;
  constants.residual.ChromaStrength = residual.chroma_strength;
  constants.residual.EditGamma = residual.edit_gamma;
  constants.residual.HighlightProtect = residual.highlight_protect;
  constants.residual.ShadowProtect = residual.shadow_protect;
  constants.residual.RatioEps = residual.ratio_eps;
  constants.residual.RatioBrighten = residual.ratio_brighten;
  constants.residual.RatioDarken = residual.ratio_darken;

  const char* pass_name = "ComposeModelOutput";

  // FSR 1.0 EASU + optional RCAS is shared by both SDR and HDR compose.
  // FsrEasuCon uses inputViewport == inputSize because the working textures
  // are exactly their allocated model resolution. con3 is derived in shader
  // from con1.y and is therefore not stored in the constant block.
  const D3D12_RESOURCE_DESC low_desc = low_enhanced->GetDesc();
  const float in_w = static_cast<float>(low_desc.Width);
  const float in_h = static_cast<float>(low_desc.Height);
  const float out_w = static_cast<float>(full_width);
  const float out_h = static_cast<float>(full_height);

  constants.easu.Con0.x = in_w / out_w;
  constants.easu.Con0.y = in_h / out_h;
  constants.easu.Con0.z = 0.5f * in_w / out_w - 0.5f;
  constants.easu.Con0.w = 0.5f * in_h / out_h - 0.5f;
  constants.easu.Con1.x = 1.0f / in_w;
  constants.easu.Con1.y = 1.0f / in_h;
  constants.easu.Con1.z = 1.0f / in_w;
  constants.easu.Con1.w = -1.0f / in_h;
  constants.easu.Con2.x = -1.0f / in_w;
  constants.easu.Con2.y = 2.0f / in_h;
  constants.easu.Con2.z = 1.0f / in_w;
  constants.easu.Con2.w = 2.0f / in_h;

  constants.rcas.Enabled = rcas_enabled ? 1.0f : 0.0f;
  constants.rcas.Strength = rcas_strength;

  if (hdr != nullptr) {
    pass_name = residual.hdr_compose_mode == HdrComposeMode::InverseToneMap
                    ? "ComposeModelOutputHdrInverse"
                    : "ComposeModelOutputHdr";
    constants.hdr.Format = hdr->format;
    constants.hdr.Exposure = hdr->exposure;
    constants.hdr.Bt2446K1 = hdr->bt2446_k1;
    constants.hdr.Bt2446K2 = hdr->bt2446_k2;
    constants.hdr.Bt2446K4 = hdr->bt2446_k4;
    constants.hdr.Crosstalk = hdr->bt2446_crosstalk;
    constants.hdr.GainMinLog2 = hdr->gain_min_log2;
    constants.hdr.GainMaxLog2 = hdr->gain_max_log2;
    constants.hdr.EffectiveSceneWhiteNits = hdr->effective_scene_white_nits;
    constants.hdr.ComposeMode =
        residual.hdr_compose_mode == HdrComposeMode::InverseToneMap ? 1.0f : 0.0f;
    constants.hdr.LocalExposure.x = hdr->local_exposure_strength;
    constants.hdr.LocalExposure.y = hdr->local_max_brighten;
    constants.hdr.LocalExposure.z = hdr->local_max_darken;
    constants.hdr.LocalExposure.w = hdr->midtone;

    if (grade_input != nullptr) {
      constants.grade.Enabled = grade_input->enabled ? 1.0f : 0.0f;
      constants.grade.Luma = grade_input->luma;
      constants.grade.Chroma = grade_input->chroma;
      constants.grade.LumaMode =
          static_cast<float>(static_cast<int>(grade_input->luma_mode));
      constants.grade.ChromaMode =
          static_cast<float>(static_cast<int>(grade_input->chroma_mode));
      constants.grade.BlackLevel = grade_input->black_level;
      constants.grade.WhiteLevel = grade_input->white_level;
      constants.grade.Contrast = grade_input->contrast;
      constants.grade.Shadows = grade_input->shadows;
      constants.grade.Highlights = grade_input->highlights;
    }
  }

  state.Transition(command_list, packed_source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, hdr_source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {
      packed_source, low_input, low_enhanced,
      hdr != nullptr ? hdr_source : packed_source};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(pass_name, command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

#include "compose/compose.h"
#include "profiling/profile.h"

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
    const StrengthSettings& strength,
    const SoftCompressionSettings& soft_compression,
    const ResidualSettings& residual,
    const GradeSettings* grade_input,
    const HdrUniforms* hdr) {
  DLSSNR_PROFILE_RENDER_L2("ComposeOutput");
  if (codec == nullptr || command_list == nullptr || packed_source == nullptr ||
      hdr_source == nullptr || low_input == nullptr || low_enhanced == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  constants.shader.Params0.x = strength.luma_strength;
  constants.shader.Params0.y = strength.chroma_strength;
  constants.shader.Params0.z = soft_compression.enabled ? 1.0f : 0.0f;
  constants.shader.Params0.w = soft_compression.max_brighten;
  constants.shader.Params1.x = soft_compression.max_darken;
  constants.shader.Params1.y = soft_compression.max_chroma;
  constants.shader.Params1.z = soft_compression.knee;
  constants.residual.EditStrength = residual.edit_strength;
  constants.residual.ChromaStrength = residual.chroma_strength;
  constants.residual.EditGamma = residual.edit_gamma;
  constants.residual.HighlightProtect = residual.highlight_protect;
  constants.residual.ShadowProtect = residual.shadow_protect;
  constants.residual.RatioEps = residual.ratio_eps;
  constants.residual.RatioBrighten = residual.ratio_brighten;
  constants.residual.RatioDarken = residual.ratio_darken;

  constants.hdr.UpsampleSharpness = residual.upsample_sharpness;

  const char* pass_name = "ComposeModelOutput";

  if (hdr != nullptr) {
    pass_name = "ComposeModelOutputHdr";
    constants.hdr.Format = hdr->format;
    constants.hdr.Exposure = hdr->exposure;
    constants.hdr.Bt2446K1 = hdr->bt2446_k1;
    constants.hdr.Bt2446K2 = hdr->bt2446_k2;
    constants.hdr.Bt2446K3 = hdr->bt2446_k3;
    constants.hdr.Bt2446K4 = hdr->bt2446_k4;
    constants.hdr.Bt2446Ip = hdr->bt2446_ip;
    constants.hdr.Crosstalk = hdr->bt2446_crosstalk;
    constants.hdr.GainMinLog2 = hdr->gain_min_log2;
    constants.hdr.GainMaxLog2 = hdr->gain_max_log2;
    constants.hdr.EffectiveSceneWhiteNits = hdr->effective_scene_white_nits;
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

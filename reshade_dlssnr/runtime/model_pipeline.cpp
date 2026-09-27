#include "model_pipeline.h"

#include <cstring>

#include "shader_params.h"

namespace reshade_dlssnr::model_pipeline {

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
    unsigned int full_width,
    unsigned int full_height,
    unsigned int work_width,
    unsigned int work_height,
    const HdrUniforms* hdr) {
  if (out_model_color == nullptr || out_model_output == nullptr ||
      codec == nullptr || command_list == nullptr || full_input == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = work_width;
  constants.Height = work_height;
  (void)full_width;
  (void)full_height;

  const char* pass_name = "PrepareModelInput";
  if (hdr != nullptr) {
    pass_name = "TonemapForward";
    constants.params[0] = hdr->exposure;
    constants.params[1] = hdr->white_point;
    constants.params[2] = static_cast<float>(hdr->format);
  }

  state.Transition(command_list, full_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {full_input, full_input, full_input};
  ID3D12Resource* uavs[kUavCount] = {resources.GetWorkingInput()};

  codec->Dispatch(pass_name, command_list, constants, srvs, uavs);

  state.Transition(command_list, resources.GetWorkingInput(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  *out_model_color = resources.GetWorkingInput();
  *out_model_output = resources.GetWorkingOutput();
  *out_eval_width = work_width;
  *out_eval_height = work_height;
  return true;
}

bool ComposeOutput(
    CommandContext& state,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    ComputeEngine* codec,
    ID3D12Resource* original,
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
  if (codec == nullptr || command_list == nullptr || original == nullptr ||
      low_input == nullptr || low_enhanced == nullptr || params == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = full_width;
  constants.Height = full_height;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    constants.params[kShaderParamSlotBase + i] = params[i];
  }
  // Written for both paths. How the edit is carried back is not an HDR
  // concern; it only used to be stored here.
  constants.params[kResidualSlotBase + 0] = residual.edit_strength;
  constants.params[kResidualSlotBase + 1] = residual.chroma_strength;
  constants.params[kResidualSlotBase + 2] = residual.edit_gamma;
  constants.params[kResidualSlotBase + 3] = residual.highlight_protect;
  constants.params[kResidualSlotBase + 4] = residual.shadow_protect;

  const char* pass_name = "ComposeModelOutput";
  if (hdr != nullptr) {
    pass_name = "ComposeModelOutputHdr";
    // Exposure and white point are read by the compose so it can invert the
    // tonemap on the model's output. The model saw the tonemapped image, so
    // the ratio it implies is in the tonemapped domain.
    constants.params[kHdrSlotBase + 0] = static_cast<float>(hdr->format);
    constants.params[kHdrSlotBase + 1] = hdr->exposure;
    constants.params[kHdrSlotBase + 2] = hdr->white_point;

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

    // The input grade the model's image was put through, so the compose can
    // apply it to the tonemapped source and invert it after the ratio.
    if (grade_input != nullptr) {
      constants.params[kGradeSlotBase + 0] = grade_input->enabled ? 1.0f : 0.0f;
      constants.params[kGradeSlotBase + 1] = grade_input->luma;
      constants.params[kGradeSlotBase + 2] = grade_input->chroma;
      constants.params[kGradeSlotBase + 3] =
          static_cast<float>(static_cast<int>(grade_input->luma_mode));
      constants.params[kGradeSlotBase + 4] =
          static_cast<float>(static_cast<int>(grade_input->chroma_mode));
      constants.params[kGradeSlotBase + 5] = grade_input->black_point;
      constants.params[kGradeSlotBase + 6] = grade_input->white_point;
      constants.params[kGradeSlotBase + 7] = grade_input->contrast;
      constants.params[kGradeSlotBase + 8] = grade_input->shadows;
      constants.params[kGradeSlotBase + 9] = grade_input->highlights;
    }
  }

  state.Transition(command_list, original,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_input,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, low_enhanced,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ID3D12Resource* srvs[kSrvCount] = {original, low_input, low_enhanced};
  ID3D12Resource* uavs[kUavCount] = {resources.GetCompositeOutput()};

  return codec->Dispatch(pass_name, command_list, constants, srvs, uavs);
}

bool ApplyGrade(CommandContext& state,
                ID3D12GraphicsCommandList* command_list,
                ComputeEngine* codec,
                ID3D12Resource* target,
                ID3D12Resource* scratch,
                unsigned int width,
                unsigned int height,
                HdrFormat format,
                const GradeSettings& grade) {
  // A disabled grade costs nothing and needs no resources, so callers that
  // would rather not branch on it do not have to.
  if (!grade.enabled) return true;

  if (codec == nullptr || command_list == nullptr || target == nullptr ||
      scratch == nullptr) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.params[kGradeSlotBase + 0] = grade.luma;
  constants.params[kGradeSlotBase + 1] = grade.chroma;
  constants.params[kGradeSlotBase + 2] =
      static_cast<float>(static_cast<int>(grade.luma_mode));
  constants.params[kGradeSlotBase + 3] =
      static_cast<float>(static_cast<int>(grade.chroma_mode));
  constants.params[kGradeSlotBase + 4] = grade.black_point;
  constants.params[kGradeSlotBase + 5] = grade.white_point;
  constants.params[kGradeSlotBase + 6] = grade.contrast;
  constants.params[kGradeSlotBase + 7] = grade.shadows;
  constants.params[kGradeSlotBase + 8] = grade.highlights;
  constants.params[kGradeSlotBase + 9] =
      static_cast<float>(HdrFormatCode(format));

  // Same shape as the input snapshot in StepResolveModelInput: park the source
  // in COPY_SOURCE, land the copy in the scratch, then read the scratch and
  // write back over the original.
  state.Transition(command_list, target, D3D12_RESOURCE_STATE_COPY_SOURCE);
  state.Transition(command_list, scratch, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(scratch, target);
  state.Transition(command_list, scratch,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {scratch, scratch, scratch};
  ID3D12Resource* uavs[kUavCount] = {target};

  return codec->Dispatch("ApplyGrade", command_list, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr::model_pipeline

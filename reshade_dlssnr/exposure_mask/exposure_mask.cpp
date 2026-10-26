#include "exposure_mask/exposure_mask.h"
#include "profiling/profile.h"

#include <algorithm>
#include <utility>

#include <reshade.hpp>

#include "gpu/gpu_resources.h"
#include "pipeline/pass_session.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {
namespace {

// Mask textures are plain R8 UAVs that start out as SRVs.
D3D12_RESOURCE_DESC MaskDesc(unsigned int width, unsigned int height) {
  return Texture2DDesc(width, height, DXGI_FORMAT_R8_UNORM,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}

PipelineStatus RunBuildExposureMask(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("RunBuildExposureMask");
  // Compare the exact post-preparation, post-input-grade resource sent to NGX.
  // The builder is scheduled only after this pass's output barrier. A failed
  // NGX evaluation must not publish a mask derived from stale output memory.
  if (run.stage == nullptr || run.stage->owns_submit ||
      (!run.stage->is_pre_dlss && !run.stage->is_post_dlss) ||
      !run.hdr_state.enable || !run.mask_state.enable ||
      run.exposure_mask == nullptr ||
      run.model_color == nullptr || run.model_output == nullptr ||
      run.eval_result != NVSDK_NGX_Result_Success || run.temporal_reuse ||
      run.session == nullptr ||
      run.cmd == nullptr || run.codec == nullptr ||
      run.device == nullptr || run.hdr == nullptr || !run.hdr->Enabled() ||
      !run.exposure_mask->CanBuildThisFrame()) {
    return PipelineStatus::Ok;
  }

  if (!run.exposure_mask->Build(
          *run.cmd, *run.codec, run.session->StageList(run, *run.cmd),
          run.device, run.model_color, run.model_output,
          run.mask_state)) {
    TRACE_EVT(run.session, "exposure_mask BUILD_SKIP");
  }
  return PipelineStatus::Ok;
}

constexpr PipelineNodeId kExposureMaskRequires[] = {
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::Count};

constexpr PipelineStepDef kExposureMaskSteps[] = {
    {PipelineNodeId::BuildExposureMask, "BuildExposureMask",
     &RunBuildExposureMask, kExposureMaskRequires,
     StageInput::CommandList | StageInput::Stage, /*requires_hdr_aware=*/true},
};

static_assert(
    PrerequisitesPrecedeSteps(kPreDlssStepNodes,
                              ArrayCount(kPreDlssStepNodes),
                              kExposureMaskSteps,
                              ArrayCount(kExposureMaskSteps)),
    "Exposure Mask requires ModelOutputBarrier before its build step");
static_assert(
    PrerequisitesPrecedeSteps(kPostDlssStepNodes,
                              ArrayCount(kPostDlssStepNodes),
                              kExposureMaskSteps,
                              ArrayCount(kExposureMaskSteps)),
    "Exposure Mask requires ModelOutputBarrier before its build step");
static_assert(
    StageInputsCoverSteps(kPreDlssStepNodes, ArrayCount(kPreDlssStepNodes),
                          kExposureMaskSteps, ArrayCount(kExposureMaskSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "Exposure Mask requires inputs the Pre-DLSS stage does not provide");
static_assert(
    StageInputsCoverSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                          kExposureMaskSteps, ArrayCount(kExposureMaskSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "Exposure Mask requires inputs the Post-DLSS stage does not provide");

}  // namespace

void ExposureMask::Shutdown() {
  DLSSNR_PROFILE_RESOURCE_L2("ExposureMask::Shutdown");
  masks_[0].Reset();
  masks_[1].Reset();
  classifier_.Reset();
  frame_index_ = 0;
  latest_index_ = 0;
  frame_valid_ = false;
  latest_valid_ = false;
  built_this_frame_ = false;
  generated_this_frame_ = false;
}

void ExposureMask::AdvanceFrame(bool enabled) {
  DLSSNR_PROFILE_CORE_L2("ExposureMask::AdvanceFrame");
  built_this_frame_ = false;
  generated_this_frame_ = false;
  if (!enabled) {
    frame_valid_ = false;
    latest_valid_ = false;
    return;
  }

  if (latest_valid_) {
    frame_index_ = latest_index_;
    frame_valid_ = true;
  } else {
    frame_valid_ = false;
  }
}

ExposureMask::Output ExposureMask::GetFrameOutput() const {
  if (!frame_valid_ || masks_[frame_index_] == nullptr) return {};
  return {masks_[frame_index_].Get(), masks_[frame_index_].Width(),
          masks_[frame_index_].Height(), true};
}

ExposureMask::Output ExposureMask::GetGeneratedOutput() const {
  if (!generated_this_frame_ || !latest_valid_ ||
      masks_[latest_index_] == nullptr) {
    return {};
  }
  return {masks_[latest_index_].Get(), masks_[latest_index_].Width(),
          masks_[latest_index_].Height(), true};
}

bool ExposureMask::EnsureResources(ID3D12Device* device,
                                     unsigned int width,
                                     unsigned int height,
                                     uint32_t destination_index) {
  if (device == nullptr || width == 0 || height == 0 ||
      destination_index > 1u) {
    return false;
  }

  // Never resize the published snapshot while a frame is in progress. Only
  // the destination is replaced on a working-resolution change; the shader
  // maps the previous mask by normalized coordinates for temporal filtering.
  if (!masks_[destination_index].Ensure(
          device, MaskDesc(width, height),
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "exposure_mask")) {
    return false;
  }
  if (!classifier_.Ensure(device, MaskDesc(width, height),
                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                 "exposure_mask_classifier")) {
    return false;
  }

  // Before a valid snapshot exists, keep a matching resource for the unused
  // history binding. Once frame_valid_ is true, this slot is immutable until
  // Present, even if its dimensions differ from the next destination.
  if (!frame_valid_ && !masks_[frame_index_].Matches(device, width, height,
                                                     DXGI_FORMAT_R8_UNORM)) {
    if (!masks_[frame_index_].Ensure(
            device, MaskDesc(width, height),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "exposure_mask")) {
      return false;
    }
  }
  return masks_[frame_index_] != nullptr;
}

bool ExposureMask::Build(CommandContext& command,
                           ComputeEngine& codec,
                           ID3D12GraphicsCommandList* command_list,
                           ID3D12Device* device,
                           ID3D12Resource* model_input,
                           ID3D12Resource* model_output,
                           const ExposureMaskSettings& settings) {
  if (built_this_frame_) return true;
  if (command_list == nullptr || device == nullptr || model_input == nullptr ||
      model_output == nullptr) {
    return false;
  }

  const D3D12_RESOURCE_DESC input_desc = model_input->GetDesc();
  const D3D12_RESOURCE_DESC output_desc = model_output->GetDesc();
  if (input_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      output_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      input_desc.SampleDesc.Count != 1 || output_desc.SampleDesc.Count != 1 ||
      input_desc.Width != output_desc.Width ||
      input_desc.Height != output_desc.Height || input_desc.Width == 0 ||
      input_desc.Height == 0) {
    return false;
  }

  const unsigned int width = static_cast<unsigned int>(input_desc.Width);
  const unsigned int height = input_desc.Height;

  // The published frame mask is immutable until Present. Always write the
  // other texture so later passes cannot observe this frame's generated mask.
  const uint32_t destination_index = 1u - frame_index_;
  if (!EnsureResources(device, width, height, destination_index)) return false;
  ID3D12Resource* previous = masks_[frame_index_].Get();
  ID3D12Resource* destination = masks_[destination_index].Get();

  command.SetResourceState(masks_[0].Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.SetResourceState(masks_[1].Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command_list, model_input,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command_list, model_output,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command_list, previous,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command_list, destination,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ComputeConstants classify_constants = {};
  classify_constants.Width = width;
  classify_constants.Height = height;
  classify_constants.shader.Params0.x = settings.luma_threshold;
  classify_constants.shader.Params0.y = settings.luma_relative;
  classify_constants.shader.Params0.z = settings.chroma_threshold;
  classify_constants.shader.Params1.x = settings.edit_strength;
  classify_constants.shader.Params1.w = frame_valid_ ? 1.0f : 0.0f;

  command.SetResourceState(classifier_.Get(),
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  command.Transition(command_list, classifier_.Get(),
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* classify_srvs[kSrvCount] = {
      model_input, model_output, previous, model_input};
  ID3D12Resource* classify_uavs[kUavCount] = {classifier_.Get()};
  if (!codec.Dispatch("ClassifyExposureMask", command_list,
                      classify_constants, classify_srvs, classify_uavs)) {
    command.Transition(command_list, classifier_.Get(),
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    command.Transition(command_list, destination,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return false;
  }
  command.UavBarrier(command_list, classifier_.Get());
  command.Transition(command_list, classifier_.Get(),
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ComputeConstants smooth_constants = {};
  smooth_constants.Width = width;
  smooth_constants.Height = height;
  smooth_constants.shader.Params1.y = settings.mask_smoothing;
  smooth_constants.shader.Params1.z = 0.02f + settings.mask_response * 0.98f;
  smooth_constants.shader.Params1.w = frame_valid_ ? 1.0f : 0.0f;
  ID3D12Resource* smooth_srvs[kSrvCount] = {
      classifier_.Get(), classifier_.Get(), previous, classifier_.Get()};
  ID3D12Resource* smooth_uavs[kUavCount] = {destination};
  if (!codec.Dispatch("BuildExposureMask", command_list,
                      smooth_constants, smooth_srvs, smooth_uavs)) {
    command.Transition(command_list, destination,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return false;
  }
  command.UavBarrier(command_list, destination);
  command.Transition(command_list, destination,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  latest_index_ = destination_index;
  latest_valid_ = true;
  built_this_frame_ = true;
  generated_this_frame_ = true;
  return true;
}

const PipelineStepDef* ExposureMaskStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kExposureMaskSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr

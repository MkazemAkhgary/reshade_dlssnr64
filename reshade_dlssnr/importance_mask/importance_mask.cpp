#include "importance_mask/importance_mask.h"

#include <algorithm>
#include <utility>

#include <reshade.hpp>

#include "dlss_guides.h"
#include "pipeline/pass_session.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {
namespace {

void RetireToGuides(ID3D12Resource* resource) {
  if (resource == nullptr) return;
  resource->AddRef();
  DlssGuides::Get().RetireGameResource(resource);
  resource->Release();
}

void RetireSlot(Microsoft::WRL::ComPtr<ID3D12Resource>& slot) {
  if (slot != nullptr) RetireToGuides(slot.Detach());
}

bool CreateMaskResource(ID3D12Device* device,
                        unsigned int width,
                        unsigned int height,
                        Microsoft::WRL::ComPtr<ID3D12Resource>& output) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
             &heap, D3D12_HEAP_FLAG_NONE, &desc,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
             IID_PPV_ARGS(&output))) && output != nullptr;
}

PipelineStatus RunBuildImportanceMask(PassRun& run) {
  // Compare the exact post-preparation, post-input-grade resource sent to NGX.
  // The builder is scheduled only after this pass's output barrier. A failed
  // NGX evaluation must not publish a mask derived from stale output memory.
  if (run.stage == nullptr || run.stage->owns_submit ||
      (!run.stage->is_pre_dlss && !run.stage->is_post_dlss) ||
      run.settings == nullptr || !run.settings->hdr.enable ||
      !run.settings->importance_mask.enable || run.importance_mask == nullptr ||
      run.model_color == nullptr || run.model_output == nullptr ||
      run.eval_result != NVSDK_NGX_Result_Success || run.session == nullptr ||
      run.cmd == nullptr || run.codec == nullptr ||
      run.device == nullptr || run.hdr == nullptr || !run.hdr->Enabled() ||
      !run.importance_mask->CanBuildThisFrame()) {
    return PipelineStatus::Ok;
  }

  if (!run.importance_mask->Build(
          *run.cmd, *run.codec, run.session->StageList(run, *run.cmd),
          run.device, run.model_color, run.model_output,
          run.settings->importance_mask)) {
    TRACE_EVT(run.session, "importance_mask BUILD_SKIP");
  }
  return PipelineStatus::Ok;
}

constexpr PipelineNodeId kImportanceMaskRequires[] = {
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::Count};

constexpr PipelineStepDef kImportanceMaskSteps[] = {
    {PipelineNodeId::BuildImportanceMask, "BuildImportanceMask",
     &RunBuildImportanceMask, kImportanceMaskRequires,
     StageInput::CommandList | StageInput::Stage, /*requires_hdr_aware=*/true},
};

static_assert(
    PrerequisitesPrecedeSteps(kPreDlssStepNodes,
                              ArrayCount(kPreDlssStepNodes),
                              kImportanceMaskSteps,
                              ArrayCount(kImportanceMaskSteps)),
    "Importance Mask requires ModelOutputBarrier before its build step");
static_assert(
    PrerequisitesPrecedeSteps(kPostDlssStepNodes,
                              ArrayCount(kPostDlssStepNodes),
                              kImportanceMaskSteps,
                              ArrayCount(kImportanceMaskSteps)),
    "Importance Mask requires ModelOutputBarrier before its build step");
static_assert(
    StageInputsCoverSteps(kPreDlssStepNodes, ArrayCount(kPreDlssStepNodes),
                          kImportanceMaskSteps, ArrayCount(kImportanceMaskSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "Importance Mask requires inputs the Pre-DLSS stage does not provide");
static_assert(
    StageInputsCoverSteps(kPostDlssStepNodes, ArrayCount(kPostDlssStepNodes),
                          kImportanceMaskSteps, ArrayCount(kImportanceMaskSteps),
                          StageInput::CommandList | StageInput::OutputResource |
                              StageInput::Stage),
    "Importance Mask requires inputs the Post-DLSS stage does not provide");

}  // namespace

void ImportanceMask::Shutdown() {
  RetireSlot(masks_[0]);
  RetireSlot(masks_[1]);
  frame_index_ = 0;
  latest_index_ = 0;
  mask_width_[0] = mask_width_[1] = 0;
  mask_height_[0] = mask_height_[1] = 0;
  frame_valid_ = false;
  latest_valid_ = false;
  built_this_frame_ = false;
  generated_this_frame_ = false;
}

void ImportanceMask::AdvanceFrame(bool enabled) {
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

ImportanceMask::Output ImportanceMask::GetFrameOutput() const {
  if (!frame_valid_ || masks_[frame_index_] == nullptr) return {};
  return {masks_[frame_index_].Get(), mask_width_[frame_index_],
          mask_height_[frame_index_], true};
}

ImportanceMask::Output ImportanceMask::GetGeneratedOutput() const {
  if (!generated_this_frame_ || !latest_valid_ ||
      masks_[latest_index_] == nullptr) {
    return {};
  }
  return {masks_[latest_index_].Get(), mask_width_[latest_index_],
          mask_height_[latest_index_], true};
}

bool ImportanceMask::EnsureResources(ID3D12Device* device,
                                     unsigned int width,
                                     unsigned int height,
                                     uint32_t destination_index) {
  if (device == nullptr || width == 0 || height == 0 ||
      destination_index > 1u) {
    return false;
  }

  const auto ensure_slot = [&](uint32_t index) {
    if (masks_[index] != nullptr && mask_width_[index] == width &&
        mask_height_[index] == height) {
      return true;
    }

    Microsoft::WRL::ComPtr<ID3D12Resource> next;
    if (!CreateMaskResource(device, width, height, next)) return false;

    RetireSlot(masks_[index]);
    masks_[index] = std::move(next);
    mask_width_[index] = width;
    mask_height_[index] = height;
    return true;
  };

  // Never resize the published snapshot while a frame is in progress. Only
  // the destination is replaced on a working-resolution change; the shader
  // maps the previous mask by normalized coordinates for temporal filtering.
  if (!ensure_slot(destination_index)) return false;

  // Before a valid snapshot exists, keep a matching resource for the unused
  // history binding. Once frame_valid_ is true, this slot is immutable until
  // Present, even if its dimensions differ from the next destination.
  if (!frame_valid_ &&
      (masks_[frame_index_] == nullptr || mask_width_[frame_index_] != width ||
       mask_height_[frame_index_] != height)) {
    if (!ensure_slot(frame_index_)) return false;
  }
  return masks_[frame_index_] != nullptr;
}

bool ImportanceMask::Build(CommandContext& command,
                           ComputeEngine& codec,
                           ID3D12GraphicsCommandList* command_list,
                           ID3D12Device* device,
                           ID3D12Resource* model_input,
                           ID3D12Resource* model_output,
                           const ImportanceMaskSettings& settings) {
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

  ComputeConstants constants = {};
  constants.Width = width;
  constants.Height = height;
  constants.shader.Params0.x = settings.luma_threshold;
  constants.shader.Params0.y = settings.luma_relative;
  constants.shader.Params0.z = settings.chroma_threshold;
  constants.shader.Params1.x = settings.edit_strength;
  constants.shader.Params1.y = settings.mask_smoothing;
  constants.shader.Params1.z = 0.02f + settings.mask_response * 0.98f;
  constants.shader.Params1.w = frame_valid_ ? 1.0f : 0.0f;

  ID3D12Resource* srvs[kSrvCount] = {
      model_input, model_output, previous, model_input};
  ID3D12Resource* uavs[kUavCount] = {destination};
  if (!codec.Dispatch("BuildImportanceMask", command_list, constants, srvs,
                      uavs)) {
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

const PipelineStepDef* ImportanceMaskStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kImportanceMaskSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

}  // namespace reshade_dlssnr

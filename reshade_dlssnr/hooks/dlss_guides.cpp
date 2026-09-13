#include "dlss_guides.h"

#include <reshade.hpp>

#include "d3d12_utils.h"

namespace reshade_dlssnr {

namespace {

ID3D12Resource* GetResource(const NVSDK_NGX_Parameter* params, const char* a, const char* b) {
  ID3D12Resource* resource = nullptr;
  if (params->Get(a, &resource) == NVSDK_NGX_Result_Success && resource != nullptr) {
    return resource;
  }

  resource = nullptr;
  if (params->Get(b, &resource) == NVSDK_NGX_Result_Success && resource != nullptr) {
    return resource;
  }

  void* untyped = nullptr;
  if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }

  untyped = nullptr;
  if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }

  return nullptr;
}

}  // namespace

DlssGuides& DlssGuides::Get() {
  static DlssGuides guides;
  return guides;
}

bool DlssGuides::IsTypelessFormat(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return true;
    default:
      return false;
  }
}

DXGI_FORMAT DlssGuides::TypedGuideFormat(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

void DlssGuides::Barrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  if (before == after || resource == nullptr) return;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

ID3D12Resource* DlssGuides::CreateGuideClone(
    ID3D12Device* device,
    ID3D12Resource* source,
    unsigned int* out_width,
    unsigned int* out_height,
    DXGI_FORMAT* out_format) {
  D3D12_RESOURCE_DESC desc = source->GetDesc();

  // Preserve the original format so CopyResource is valid. Typeless formats
  // are converted to their typed counterpart so they can be used as SRVs.
  if (IsTypelessFormat(desc.Format)) {
    desc.Format = TypedGuideFormat(desc.Format);
  }

  desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  ID3D12Resource* resource = nullptr;
  if (FAILED(device->CreateCommittedResource(
          &heap,
          D3D12_HEAP_FLAG_NONE,
          &desc,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          nullptr,
          IID_PPV_ARGS(&resource)))) {
    return nullptr;
  }

  if (out_width != nullptr) *out_width = static_cast<unsigned int>(desc.Width);
  if (out_height != nullptr) *out_height = static_cast<unsigned int>(desc.Height);
  if (out_format != nullptr) *out_format = desc.Format;

  return resource;
}

void DlssGuides::Capture(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id) {
  std::lock_guard<std::mutex> lock(lock_);

  if (feature_id != NVSDK_NGX_Feature_SuperSampling || command_list == nullptr || params == nullptr) {
    state_.valid = false;
    return;
  }

  ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
  ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

  if (depth == nullptr || motion == nullptr) {
    state_.valid = false;
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    state_.valid = false;
    return;
  }

  unsigned int guide_width = 0;
  unsigned int guide_height = 0;
  params->Get(NVSDK_NGX_Parameter_Width, &guide_width);
  params->Get(NVSDK_NGX_Parameter_Height, &guide_height);

  if (guide_width == 0 || guide_height == 0) {
    D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
    guide_width = static_cast<unsigned int>(depth_desc.Width);
    guide_height = static_cast<unsigned int>(depth_desc.Height);
  }

  unsigned int create_flags = 0;
  params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &create_flags);

  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale_x) != NVSDK_NGX_Result_Success) {
    mv_scale_x = 1.0f;
  }
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale_y) != NVSDK_NGX_Result_Success) {
    mv_scale_y = 1.0f;
  }

  // Always clone guides into owned resources. The game may release or reuse
  // the originals before the present event, and we must not hold raw pointers
  // across that boundary.
  if (depth_clone_ == nullptr ||
      depth_width_ != static_cast<unsigned int>(depth->GetDesc().Width) ||
      depth_height_ != static_cast<unsigned int>(depth->GetDesc().Height) ||
      depth_format_ != (IsTypelessFormat(depth->GetDesc().Format)
                            ? TypedGuideFormat(depth->GetDesc().Format)
                            : depth->GetDesc().Format)) {
    RetireResource(depth_clone_);
    depth_clone_.Attach(
        CreateGuideClone(device, depth, &depth_width_, &depth_height_, &depth_format_));
  }

  if (motion_clone_ == nullptr ||
      motion_width_ != static_cast<unsigned int>(motion->GetDesc().Width) ||
      motion_height_ != static_cast<unsigned int>(motion->GetDesc().Height) ||
      motion_format_ != (IsTypelessFormat(motion->GetDesc().Format)
                             ? TypedGuideFormat(motion->GetDesc().Format)
                             : motion->GetDesc().Format)) {
    RetireResource(motion_clone_);
    motion_clone_.Attach(
        CreateGuideClone(device, motion, &motion_width_, &motion_height_, &motion_format_));
  }

  if (depth_clone_ == nullptr || motion_clone_ == nullptr) {
    state_.valid = false;
    device->Release();
    return;
  }

  const D3D12_RESOURCE_STATES depth_state =
      reshade_dlssnr::InferResourceState(depth);
  Barrier(command_list, depth, depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, depth_clone_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(depth_clone_.Get(), depth);
  Barrier(command_list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depth_state);
  Barrier(command_list, depth_clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  const D3D12_RESOURCE_STATES motion_state =
      reshade_dlssnr::InferResourceState(motion);
  Barrier(command_list, motion, motion_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, motion_clone_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(motion_clone_.Get(), motion);
  Barrier(command_list, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, motion_state);
  Barrier(command_list, motion_clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // Clone the DLSS output color for use as model input at Post-DLSS.
  ID3D12Resource* output = GetResource(
      params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
  const bool output_fresh = (output != nullptr);

  if (output != nullptr) {
    if (dlss_output_clone_ == nullptr ||
        dlss_output_width_ != static_cast<unsigned int>(output->GetDesc().Width) ||
        dlss_output_height_ != static_cast<unsigned int>(output->GetDesc().Height) ||
        dlss_output_format_ != (IsTypelessFormat(output->GetDesc().Format)
                                    ? TypedGuideFormat(output->GetDesc().Format)
                                    : output->GetDesc().Format)) {
      RetireResource(dlss_output_clone_);
      dlss_output_clone_.Attach(CreateGuideClone(
          device, output, &dlss_output_width_, &dlss_output_height_,
          &dlss_output_format_));
    }

    if (dlss_output_clone_ != nullptr) {
      const D3D12_RESOURCE_STATES output_state =
          reshade_dlssnr::InferResourceState(output);
      Barrier(command_list, output, output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Barrier(command_list, dlss_output_clone_.Get(),
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_COPY_DEST);
      command_list->CopyResource(dlss_output_clone_.Get(), output);
      Barrier(command_list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, output_state);
      Barrier(command_list, dlss_output_clone_.Get(),
              D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  state_.depth = depth_clone_.Get();
  state_.motion = motion_clone_.Get();
  // Only advertise the output clone when we actually refreshed it this frame.
  // Otherwise the Post-DLSS path would run on stale data from a previous frame.
  state_.dlss_output_color = output_fresh ? dlss_output_clone_.Get() : nullptr;
  state_.guide_width = guide_width;
  state_.guide_height = guide_height;
  state_.output_width = dlss_output_width_;
  state_.output_height = dlss_output_height_;
  state_.mv_scale_x = mv_scale_x;
  state_.mv_scale_y = mv_scale_y;
  state_.depth_inverted = (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  state_.valid = true;

  device->Release();
}

void DlssGuides::RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
  if (resource == nullptr) return;
  retired_resources_.push_back(resource.Detach());
}

DlssGuideState DlssGuides::GetState() const {
  std::lock_guard<std::mutex> lock(lock_);
  return state_;
}

void DlssGuides::ReleaseRetiredResourcesLocked() {
  for (ID3D12Resource* resource : retired_resources_) {
    if (resource != nullptr) {
      resource->Release();
    }
  }

  retired_resources_.clear();
}

void DlssGuides::ReleaseRetiredGameResourcesLocked() {
  for (ID3D12Resource* resource : retired_game_resources_) {
    if (resource != nullptr) {
      resource->Release();
    }
  }

  retired_game_resources_.clear();
}

void DlssGuides::ReleaseRetiredResources() {
  std::lock_guard<std::mutex> lock(lock_);
  ReleaseRetiredResourcesLocked();
}

void DlssGuides::RetireGameResource(ID3D12Resource* resource) {
  if (resource == nullptr) return;
  std::lock_guard<std::mutex> lock(lock_);
  retired_game_resources_.push_back(resource);
}

void DlssGuides::ReleaseRetiredGameResources() {
  std::lock_guard<std::mutex> lock(lock_);
  ReleaseRetiredGameResourcesLocked();
}

void DlssGuides::Clear() {
  std::lock_guard<std::mutex> lock(lock_);

  depth_clone_.Reset();
  motion_clone_.Reset();
  dlss_output_clone_.Reset();

  ReleaseRetiredResourcesLocked();
  ReleaseRetiredGameResourcesLocked();

  state_.valid = false;
  depth_width_ = 0;
  depth_height_ = 0;
  depth_format_ = DXGI_FORMAT_UNKNOWN;
  motion_width_ = 0;
  motion_height_ = 0;
  motion_format_ = DXGI_FORMAT_UNKNOWN;
  dlss_output_width_ = 0;
  dlss_output_height_ = 0;
  dlss_output_format_ = DXGI_FORMAT_UNKNOWN;
}

}  // namespace reshade_dlssnr

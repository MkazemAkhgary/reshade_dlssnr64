#include "dlss_guides.h"

#include <reshade.hpp>

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
  lock_.lock();

  if (feature_id != NVSDK_NGX_Feature_SuperSampling || command_list == nullptr || params == nullptr) {
    state_.valid = false;
    lock_.unlock();
    return;
  }

  ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
  ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

  if (depth == nullptr || motion == nullptr) {
    state_.valid = false;
    lock_.unlock();
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    state_.valid = false;
    lock_.unlock();
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
    if (depth_clone_ != nullptr) {
      depth_clone_->Release();
      depth_clone_ = nullptr;
    }

    depth_clone_ = CreateGuideClone(device, depth, &depth_width_, &depth_height_, &depth_format_);
  }

  if (motion_clone_ == nullptr ||
      motion_width_ != static_cast<unsigned int>(motion->GetDesc().Width) ||
      motion_height_ != static_cast<unsigned int>(motion->GetDesc().Height) ||
      motion_format_ != (IsTypelessFormat(motion->GetDesc().Format)
                             ? TypedGuideFormat(motion->GetDesc().Format)
                             : motion->GetDesc().Format)) {
    if (motion_clone_ != nullptr) {
      motion_clone_->Release();
      motion_clone_ = nullptr;
    }

    motion_clone_ = CreateGuideClone(device, motion, &motion_width_, &motion_height_, &motion_format_);
  }

  if (depth_clone_ == nullptr || motion_clone_ == nullptr) {
    state_.valid = false;
    device->Release();
    lock_.unlock();
    return;
  }

  Barrier(command_list, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, depth_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(depth_clone_, depth);
  Barrier(command_list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(command_list, depth_clone_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  Barrier(command_list, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, motion_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(motion_clone_, motion);
  Barrier(command_list, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Barrier(command_list, motion_clone_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  // Capture the DLSS output color before UI/post-processing for the UI mask.
  ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");

  if (target != nullptr) {
    if (pre_ui_clone_ == nullptr ||
        pre_ui_width_ != static_cast<unsigned int>(target->GetDesc().Width) ||
        pre_ui_height_ != static_cast<unsigned int>(target->GetDesc().Height) ||
        pre_ui_format_ != (IsTypelessFormat(target->GetDesc().Format)
                               ? TypedGuideFormat(target->GetDesc().Format)
                               : target->GetDesc().Format)) {
      if (pre_ui_clone_ != nullptr) {
        pre_ui_clone_->Release();
        pre_ui_clone_ = nullptr;
      }

      pre_ui_clone_ = CreateGuideClone(device, target, &pre_ui_width_, &pre_ui_height_, &pre_ui_format_);
    }

    if (pre_ui_clone_ != nullptr) {
      Barrier(command_list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
      Barrier(command_list, pre_ui_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
      command_list->CopyResource(pre_ui_clone_, target);
      Barrier(command_list, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      Barrier(command_list, pre_ui_clone_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  state_.depth = depth_clone_;
  state_.motion = motion_clone_;
  state_.pre_ui_color = pre_ui_clone_;
  state_.guide_width = guide_width;
  state_.guide_height = guide_height;
  state_.color_width = pre_ui_width_;
  state_.color_height = pre_ui_height_;
  state_.mv_scale_x = mv_scale_x;
  state_.mv_scale_y = mv_scale_y;
  state_.depth_inverted = (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  state_.valid = true;

  device->Release();
  lock_.unlock();
}

void DlssGuides::SetSelectedPassHandle(uint64_t handle) {
  std::lock_guard<std::mutex> lock(lock_);
  selected_pass_handle_ = handle;
  has_selected_pass_handle_ = handle != 0;
}

void DlssGuides::CaptureSelectedPassIfNeeded(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* selected_pass_source) {
  if (command_list == nullptr || selected_pass_source == nullptr) return;

  lock_.lock();

  if (!has_selected_pass_handle_ ||
      reinterpret_cast<uint64_t>(selected_pass_source) != selected_pass_handle_) {
    lock_.unlock();
    return;
  }

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    lock_.unlock();
    return;
  }

  if (selected_pass_clone_ == nullptr ||
      selected_pass_width_ != static_cast<unsigned int>(selected_pass_source->GetDesc().Width) ||
      selected_pass_height_ != static_cast<unsigned int>(selected_pass_source->GetDesc().Height) ||
      selected_pass_format_ != (IsTypelessFormat(selected_pass_source->GetDesc().Format)
                                    ? TypedGuideFormat(selected_pass_source->GetDesc().Format)
                                    : selected_pass_source->GetDesc().Format)) {
    // Do not release the previous clone here. It may still be referenced by
    // in-flight GPU work. Retiring it immediately can cause a device hang.
    selected_pass_clone_ = CreateGuideClone(
        device,
        selected_pass_source,
        &selected_pass_width_,
        &selected_pass_height_,
        &selected_pass_format_);
  }

  if (selected_pass_clone_ == nullptr) {
    device->Release();
    lock_.unlock();
    return;
  }

  Barrier(command_list, selected_pass_source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(command_list, selected_pass_clone_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(selected_pass_clone_, selected_pass_source);
  Barrier(command_list, selected_pass_source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Barrier(command_list, selected_pass_clone_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  state_.selected_pass_color = selected_pass_clone_;
  state_.selected_width = selected_pass_width_;
  state_.selected_height = selected_pass_height_;

  device->Release();
  lock_.unlock();
}

DlssGuideState DlssGuides::GetState() const {
  std::lock_guard<std::mutex> lock(lock_);
  return state_;
}

void DlssGuides::Clear() {
  std::lock_guard<std::mutex> lock(lock_);

  if (depth_clone_ != nullptr) {
    depth_clone_->Release();
    depth_clone_ = nullptr;
  }

  if (motion_clone_ != nullptr) {
    motion_clone_->Release();
    motion_clone_ = nullptr;
  }

  if (pre_ui_clone_ != nullptr) {
    pre_ui_clone_->Release();
    pre_ui_clone_ = nullptr;
  }

  // Deliberately leak guide clones until process exit. Releasing them while
  // the game may still be using them causes intermittent device removal.
  selected_pass_clone_ = nullptr;

  state_.valid = false;
  state_.pre_ui_color = nullptr;
  state_.selected_pass_color = nullptr;
  depth_width_ = 0;
  depth_height_ = 0;
  depth_format_ = DXGI_FORMAT_UNKNOWN;
  motion_width_ = 0;
  motion_height_ = 0;
  motion_format_ = DXGI_FORMAT_UNKNOWN;
  pre_ui_width_ = 0;
  pre_ui_height_ = 0;
  pre_ui_format_ = DXGI_FORMAT_UNKNOWN;
  selected_pass_width_ = 0;
  selected_pass_height_ = 0;
  selected_pass_format_ = DXGI_FORMAT_UNKNOWN;
  has_selected_pass_handle_ = false;
  selected_pass_handle_ = 0;
}

}  // namespace reshade_dlssnr

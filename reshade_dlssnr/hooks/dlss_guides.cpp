#include "dlss_guides.h"

#include <cstdio>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "streamline_compat.h"

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

namespace {

// One line per distinct set. The render size, each resource's size and the
// subrect bases decide whether a guide lines up with the game's view.
void LogGuideGeometryOnce(const NVSDK_NGX_Parameter* params,
                          const D3D12_RESOURCE_DESC& depth,
                          const D3D12_RESOURCE_DESC& motion,
                          const D3D12_RESOURCE_DESC& output) {
  auto get_uint = [params](const char* name) -> unsigned int {
    unsigned int value = 0;
    params->Get(name, &value);
    return value;
  };
  auto get_int = [params](const char* name) -> int {
    int value = 0;
    params->Get(name, &value);
    return value;
  };

  const unsigned int render_w = get_uint(NVSDK_NGX_Parameter_Width);
  const unsigned int render_h = get_uint(NVSDK_NGX_Parameter_Height);
  const int output_subrects = get_int(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects);

  struct Key {
    unsigned int render_w, render_h, depth_w, depth_h, motion_w, motion_h, output_w, output_h;
    unsigned int color_x, color_y, depth_x, depth_y, mv_x, mv_y, out_x, out_y;
    int output_subrects;
  };
  const Key key = {
      render_w, render_h,
      static_cast<unsigned int>(depth.Width), static_cast<unsigned int>(depth.Height),
      static_cast<unsigned int>(motion.Width), static_cast<unsigned int>(motion.Height),
      static_cast<unsigned int>(output.Width), static_cast<unsigned int>(output.Height),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X),
      get_uint(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y),
      get_uint(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X),
      get_uint(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y),
      output_subrects};

  static Key last = {};
  static bool logged = false;
  if (logged && key.render_w == last.render_w && key.render_h == last.render_h &&
      key.depth_w == last.depth_w && key.depth_h == last.depth_h &&
      key.motion_w == last.motion_w && key.motion_h == last.motion_h &&
      key.output_w == last.output_w && key.output_h == last.output_h &&
      key.color_x == last.color_x && key.color_y == last.color_y &&
      key.depth_x == last.depth_x && key.depth_y == last.depth_y &&
      key.mv_x == last.mv_x && key.mv_y == last.mv_y && key.out_x == last.out_x &&
      key.out_y == last.out_y && key.output_subrects == last.output_subrects) {
    return;
  }
  last = key;
  logged = true;

  char message[384];
  std::snprintf(
      message, sizeof(message),
      "DLSS guides: render=%ux%u depth=%ux%u mv=%ux%u output=%ux%u "
      "bases color=(%u,%u) depth=(%u,%u) mv=(%u,%u) out=(%u,%u) out_subrects=%d",
      render_w, render_h, key.depth_w, key.depth_h, key.motion_w, key.motion_h,
      key.output_w, key.output_h, key.color_x, key.color_y, key.depth_x,
      key.depth_y, key.mv_x, key.mv_y, key.out_x, key.out_y, output_subrects);
  reshade::log::message(reshade::log::level::info, message);
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

void DlssGuides::CaptureGuides(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id) {
  std::lock_guard<std::mutex> lock(lock_);

  // A mode request is latched exactly once per NGX evaluation. Resource
  // ownership is moved to the retire queue rather than dropped here because
  // earlier queued commands may still be reading either representation.
  const bool requested_clone_resources =
      requested_clone_resources_.load(std::memory_order_acquire);
  if (requested_clone_resources != clone_resources_) {
    RetireResource(depth_clone_);
    RetireResource(motion_clone_);
    RetireResource(dlss_output_clone_);
    if (ID3D12Resource* retired = depth_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    if (ID3D12Resource* retired = motion_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    if (ID3D12Resource* retired = dlss_output_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    depth_width_ = depth_height_ = 0;
    depth_format_ = DXGI_FORMAT_UNKNOWN;
    motion_width_ = motion_height_ = 0;
    motion_format_ = DXGI_FORMAT_UNKNOWN;
    dlss_output_width_ = dlss_output_height_ = 0;
    dlss_output_format_ = DXGI_FORMAT_UNKNOWN;
    clone_resources_ = requested_clone_resources;
  }

  // Output is a result of this evaluation, not of the previous one. Do not
  // publish a stale output if the caller doesn't perform CaptureOutput below.
  state_.dlss_output_color = nullptr;
  state_.output_width = 0;
  state_.output_height = 0;
  if (ID3D12Resource* retired = dlss_output_direct_.Detach()) {
    retired_game_resources_.push_back({retired, 0});
  }
  state_.clone_resources = clone_resources_;

  if (feature_id != NVSDK_NGX_Feature_SuperSampling ||
      command_list == nullptr || params == nullptr) {
    state_ = {};
    state_.clone_resources = clone_resources_;
    return;
  }

  ID3D12Resource* depth =
      GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
  ID3D12Resource* motion = GetResource(
      params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

  if (depth == nullptr || motion == nullptr) {
    // In direct mode the retained game resources must not keep an obsolete
    // frame's guides alive indefinitely if the game temporarily stops
    // supplying guides. In clone mode, allocations can be reused later.
    if (ID3D12Resource* retired = depth_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    if (ID3D12Resource* retired = motion_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    if (ID3D12Resource* retired = dlss_output_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
    state_ = {};
    state_.clone_resources = clone_resources_;
    return;
  }

  ID3D12Device* queried_device = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (clone_resources_) {
    // Resource creation must use the native device, not a Streamline proxy.
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&queried_device))) ||
        queried_device == nullptr) {
      state_ = {};
      state_.clone_resources = clone_resources_;
      return;
    }
    device = sl_compat::Native(queried_device);
    queried_device->Release();
    if (device == nullptr) {
      state_ = {};
      state_.clone_resources = clone_resources_;
      return;
    }
  }

  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const unsigned int guide_width = static_cast<unsigned int>(depth_desc.Width);
  const unsigned int guide_height = static_cast<unsigned int>(depth_desc.Height);
  const unsigned int motion_width = static_cast<unsigned int>(motion_desc.Width);
  const unsigned int motion_height = static_cast<unsigned int>(motion_desc.Height);

  unsigned int create_flags = 0;
  params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &create_flags);

  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mv_scale_x) !=
      NVSDK_NGX_Result_Success) {
    mv_scale_x = 1.0f;
  }
  if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mv_scale_y) !=
      NVSDK_NGX_Result_Success) {
    mv_scale_y = 1.0f;
  }

  ID3D12Resource* published_depth = depth;
  ID3D12Resource* published_motion = motion;
  if (clone_resources_) {
    const DXGI_FORMAT desired_depth_format =
        IsTypelessFormat(depth_desc.Format) ? TypedGuideFormat(depth_desc.Format)
                                            : depth_desc.Format;
    if (depth_clone_ == nullptr || depth_width_ != guide_width ||
        depth_height_ != guide_height || depth_format_ != desired_depth_format) {
      RetireResource(depth_clone_);
      depth_clone_.Attach(CreateGuideClone(
          device.Get(), depth, &depth_width_, &depth_height_, &depth_format_));
    }

    const DXGI_FORMAT desired_motion_format =
        IsTypelessFormat(motion_desc.Format) ? TypedGuideFormat(motion_desc.Format)
                                             : motion_desc.Format;
    if (motion_clone_ == nullptr || motion_width_ != motion_width ||
        motion_height_ != motion_height || motion_format_ != desired_motion_format) {
      RetireResource(motion_clone_);
      motion_clone_.Attach(CreateGuideClone(
          device.Get(), motion, &motion_width_, &motion_height_, &motion_format_));
    }

    if (depth_clone_ == nullptr || motion_clone_ == nullptr) {
      state_ = {};
      state_.clone_resources = clone_resources_;
      return;
    }

    // Match the existing external-resource state inference used by the rest
    // of the addon. The hook has no authoritative state map for game resources.
    const D3D12_RESOURCE_STATES depth_state = InferResourceState(depth);
    Barrier(command_list, depth, depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list, depth_clone_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->CopyResource(depth_clone_.Get(), depth);
    Barrier(command_list, depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depth_state);
    Barrier(command_list, depth_clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    const D3D12_RESOURCE_STATES motion_state = InferResourceState(motion);
    Barrier(command_list, motion, motion_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list, motion_clone_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->CopyResource(motion_clone_.Get(), motion);
    Barrier(command_list, motion, D3D12_RESOURCE_STATE_COPY_SOURCE, motion_state);
    Barrier(command_list, motion_clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    published_depth = depth_clone_.Get();
    published_motion = motion_clone_.Get();
  } else {
    if (depth_direct_.Get() != depth) {
      if (ID3D12Resource* retired = depth_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
      depth_direct_ = depth;
    }
    if (motion_direct_.Get() != motion) {
      if (ID3D12Resource* retired = motion_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
      motion_direct_ = motion;
    }
    published_depth = depth_direct_.Get();
    published_motion = motion_direct_.Get();
  }

  state_.depth = published_depth;
  state_.motion = published_motion;
  state_.guide_width = guide_width;
  state_.guide_height = guide_height;
  state_.motion_width = motion_width;
  state_.motion_height = motion_height;
  state_.mv_scale_x = mv_scale_x;
  state_.mv_scale_y = mv_scale_y;
  state_.depth_inverted =
      (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  state_.clone_resources = clone_resources_;
  state_.valid = true;
}

void DlssGuides::CaptureOutput(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id) {
  std::lock_guard<std::mutex> lock(lock_);

  state_.dlss_output_color = nullptr;
  state_.output_width = 0;
  state_.output_height = 0;
  if (feature_id != NVSDK_NGX_Feature_SuperSampling ||
      command_list == nullptr || params == nullptr || !state_.valid) {
    return;
  }

  ID3D12Resource* output =
      GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
  if (output == nullptr) return;

  const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
  const unsigned int output_width = static_cast<unsigned int>(output_desc.Width);
  const unsigned int output_height = static_cast<unsigned int>(output_desc.Height);
  ID3D12Resource* published_output = output;

  if (clone_resources_) {
    ID3D12Device* queried_device = nullptr;
    if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&queried_device))) ||
        queried_device == nullptr) {
      return;
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device = sl_compat::Native(queried_device);
    queried_device->Release();
    if (device == nullptr) return;

    const DXGI_FORMAT desired_format =
        IsTypelessFormat(output_desc.Format) ? TypedGuideFormat(output_desc.Format)
                                             : output_desc.Format;
    if (dlss_output_clone_ == nullptr ||
        dlss_output_width_ != output_width ||
        dlss_output_height_ != output_height ||
        dlss_output_format_ != desired_format) {
      RetireResource(dlss_output_clone_);
      dlss_output_clone_.Attach(CreateGuideClone(
          device.Get(), output, &dlss_output_width_, &dlss_output_height_,
          &dlss_output_format_));
    }
    if (dlss_output_clone_ == nullptr) return;

    // The game-resource state tracker is approximate, so use the same
    // descriptor-based inference as the previous clone path when restoring it.
    const D3D12_RESOURCE_STATES output_state = InferResourceState(output);
    Barrier(command_list, output, output_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(command_list, dlss_output_clone_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    command_list->CopyResource(dlss_output_clone_.Get(), output);
    Barrier(command_list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, output_state);
    Barrier(command_list, dlss_output_clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    published_output = dlss_output_clone_.Get();
  } else {
    if (dlss_output_direct_.Get() != output) {
      if (ID3D12Resource* retired = dlss_output_direct_.Detach()) retired_game_resources_.push_back({retired, 0});
      dlss_output_direct_ = output;
    }
    // Direct mode needs the game's just-produced output in an SRV state before
    // shared frame preparation and Post-DLSS passes read it. The Post-DLSS
    // pass restores the game's expected state when it publishes its composite.
    const D3D12_RESOURCE_STATES output_state = InferResourceState(output);
    Barrier(command_list, output, output_state,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    published_output = dlss_output_direct_.Get();
  }

  if (state_.depth != nullptr && state_.motion != nullptr) {
    LogGuideGeometryOnce(params, state_.depth->GetDesc(),
                         state_.motion->GetDesc(), output_desc);
  }
  state_.dlss_output_color = published_output;
  state_.output_width = output_width;
  state_.output_height = output_height;
  state_.clone_resources = clone_resources_;
}


void DlssGuides::RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource>& resource) {
  if (resource == nullptr) return;
  retired_resources_.push_back({resource.Detach(), 0});
}

DlssGuideState DlssGuides::GetState() const {
  std::lock_guard<std::mutex> lock(lock_);
  return state_;
}

void DlssGuides::ReleaseRetiredResourcesLocked() {
  for (const RetiredResource& retired : retired_resources_) {
    if (retired.resource != nullptr) retired.resource->Release();
  }
  retired_resources_.clear();
}

void DlssGuides::ReleaseRetiredGameResourcesLocked() {
  for (const RetiredResource& retired : retired_game_resources_) {
    if (retired.resource != nullptr) retired.resource->Release();
  }
  retired_game_resources_.clear();
}

size_t DlssGuides::ReleaseCompletedLocked(
    std::vector<RetiredResource>& resources,
    uint64_t completed_fence_value) {
  size_t released = 0;
  auto it = resources.begin();
  while (it != resources.end()) {
    if (it->resource != nullptr && it->fence_value != 0 &&
        it->fence_value <= completed_fence_value) {
      it->resource->Release();
      it = resources.erase(it);
      ++released;
    } else {
      ++it;
    }
  }
  return released;
}

void DlssGuides::ReleaseRetiredResources() {
  std::lock_guard<std::mutex> lock(lock_);
  ReleaseRetiredResourcesLocked();
}

void DlssGuides::RetireGameResource(ID3D12Resource* resource) {
  if (resource == nullptr) return;
  std::lock_guard<std::mutex> lock(lock_);
  retired_game_resources_.push_back({resource, 0});
}

void DlssGuides::ReleaseRetiredGameResources() {
  std::lock_guard<std::mutex> lock(lock_);
  ReleaseRetiredGameResourcesLocked();
}

void DlssGuides::TagRetiredResources(uint64_t fence_value) {
  if (fence_value == 0) return;
  std::lock_guard<std::mutex> lock(lock_);
  for (RetiredResource& retired : retired_resources_) {
    if (retired.fence_value == 0) retired.fence_value = fence_value;
  }
  for (RetiredResource& retired : retired_game_resources_) {
    if (retired.fence_value == 0) retired.fence_value = fence_value;
  }
}

size_t DlssGuides::ReleaseRetiredResourcesCompleted(
    uint64_t completed_fence_value) {
  std::lock_guard<std::mutex> lock(lock_);
  return ReleaseCompletedLocked(retired_resources_, completed_fence_value) +
         ReleaseCompletedLocked(retired_game_resources_, completed_fence_value);
}

size_t DlssGuides::RetiredResourceCount() const {
  std::lock_guard<std::mutex> lock(lock_);
  return retired_resources_.size();
}

size_t DlssGuides::RetiredGameResourceCount() const {
  std::lock_guard<std::mutex> lock(lock_);
  return retired_game_resources_.size();
}

void DlssGuides::Clear() {
  std::lock_guard<std::mutex> lock(lock_);

  depth_clone_.Reset();
  motion_clone_.Reset();
  dlss_output_clone_.Reset();
  depth_direct_.Reset();
  motion_direct_.Reset();
  dlss_output_direct_.Reset();
  clone_resources_ = false;
  requested_clone_resources_.store(false, std::memory_order_release);

  ReleaseRetiredResourcesLocked();
  ReleaseRetiredGameResourcesLocked();

  state_ = {};
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

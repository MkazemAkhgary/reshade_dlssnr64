#include "dlss_guides.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "gpu/gpu_resources.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

namespace {

void TransitionBarrier(
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

D3D12_RESOURCE_DESC GuideCloneDesc(ID3D12Resource* source,
                                   DXGI_FORMAT typed_format) {
  D3D12_RESOURCE_DESC desc = source->GetDesc();
  desc.Format = typed_format;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  return desc;
}

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

ID3D12Resource* GuideSlot::PublishClone(ID3D12Device* device,
                                        ID3D12GraphicsCommandList* list,
                                        ID3D12Resource* source,
                                        const char* debug_name) {
  assert(direct_.Get() == nullptr &&
         "PublishClone: a direct reference is still live; call Reset first");
  if (device == nullptr || list == nullptr || source == nullptr) return nullptr;

  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  const D3D12_RESOURCE_DESC clone_desc =
      GuideCloneDesc(source, TypedViewFormat(source_desc.Format));
  if (!clone_.Ensure(device, clone_desc,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                     debug_name)) {
    return nullptr;
  }

  const D3D12_RESOURCE_STATES source_state = InferResourceState(source);
  TransitionBarrier(list, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  TransitionBarrier(list, clone_.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(clone_.Get(), source);
  TransitionBarrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
  TransitionBarrier(list, clone_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return clone_.Get();
}

ID3D12Resource* GuideSlot::PublishDirect(
    ID3D12Resource* source, uint64_t fence_hint,
    ID3D12GraphicsCommandList* transition_list) {
  assert(clone_ == nullptr &&
         "PublishDirect: a clone is still live; call Reset first");
  if (source == nullptr) return nullptr;
  if (direct_.Get() != source) {
    direct_.Reset(fence_hint);
    direct_ = BorrowedRef::AddRef(source);
  }
  if (transition_list != nullptr) {
    const D3D12_RESOURCE_STATES source_state = InferResourceState(source);
    TransitionBarrier(transition_list, source, source_state,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  return direct_.Get();
}

void GuideSlot::Reset(uint64_t fence_hint) {
  clone_.Reset();
  direct_.Reset(fence_hint);
}

void GuideSlot::ReleaseDirect(uint64_t fence_hint) {
  direct_.Reset(fence_hint);
}

void DlssGuides::CaptureGuides(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id,
    uint64_t retirement_fence_hint) {
  DLSSNR_PROFILE_RESOURCE_L2("DlssGuides::CaptureGuides");
  // Anything pushed during this call is tagged with the queue's most
  // recently signaled fence value. If no queue has signaled yet the hint
  // is zero and the Present-time tag still fills it in; that
  // matches the previous behaviour for the very first frames, when nothing
  // can be retired in flight anyway.
  std::lock_guard<std::mutex> lock(lock_);
  const uint64_t capture_serial = ++capture_serial_;

  // A mode request is latched exactly once per NGX evaluation. Resource
  // ownership is moved to the retire queue rather than dropped here because
  // earlier queued commands may still be reading either representation.
  const bool requested_clone_resources =
      requested_clone_resources_.load(std::memory_order_acquire);
  if (requested_clone_resources != clone_resources_) {
    depth_.Reset(retirement_fence_hint);
    motion_.Reset(retirement_fence_hint);
    output_.Reset(retirement_fence_hint);
    clone_resources_ = requested_clone_resources;
  }

  // Output is a result of this evaluation, not of the previous one. Do not
  // publish a stale output if the caller doesn't perform CaptureOutput below.
  state_.dlss_output_color = nullptr;
  state_.output_width = 0;
  state_.output_height = 0;
  output_.ReleaseDirect();
  state_.clone_resources = clone_resources_;

  if (!IsUpscalerFeature(feature_id) ||
      command_list == nullptr || params == nullptr) {
    state_ = {};
    state_.clone_resources = clone_resources_;
    state_.capture_serial = capture_serial;
    return;
  }

  ID3D12Resource* depth =
      GetNgxResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
  ID3D12Resource* motion = GetNgxResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

  if (depth == nullptr || motion == nullptr) {
    static LogOnceState logged_missing_guides;
    if (ShouldLogOnce(logged_missing_guides)) {
      reshade::log::message(
          reshade::log::level::warning,
          depth == nullptr
              ? "DLSS guides: the NGX evaluation has no Depth parameter"
              : "DLSS guides: the NGX evaluation has no MotionVectors parameter");
    }
    // In direct mode the retained game resources must not keep an obsolete
    // frame's guides alive indefinitely if the game temporarily stops
    // supplying guides. In clone mode, allocations can be reused later.
    depth_.ReleaseDirect(retirement_fence_hint);
    motion_.ReleaseDirect(retirement_fence_hint);
    output_.ReleaseDirect(retirement_fence_hint);
    state_ = {};
    state_.clone_resources = clone_resources_;
    state_.capture_serial = capture_serial;
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
      state_.capture_serial = capture_serial;
      return;
    }
    device = sl_compat::Native(queried_device);
    queried_device->Release();
    if (device == nullptr) {
      state_ = {};
      state_.clone_resources = clone_resources_;
      state_.capture_serial = capture_serial;
      return;
    }
  }

  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const unsigned int guide_width = static_cast<unsigned int>(depth_desc.Width);
  const unsigned int guide_height = static_cast<unsigned int>(depth_desc.Height);
  const unsigned int motion_width = static_cast<unsigned int>(motion_desc.Width);
  const unsigned int motion_height = static_cast<unsigned int>(motion_desc.Height);
  auto get_uint = [params](const char* name, unsigned int fallback) {
    unsigned int value = 0;
    if (params->Get(name, &value) == NVSDK_NGX_Result_Success && value != 0) {
      return value;
    }
    return fallback;
  };
  const unsigned int guide_base_x = get_uint(
      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, 0u);
  const unsigned int guide_base_y = get_uint(
      NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, 0u);
  const unsigned int motion_base_x = get_uint(
      NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, 0u);
  const unsigned int motion_base_y = get_uint(
      NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, 0u);
  const unsigned int reference_width = get_uint(
      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
      get_uint(NVSDK_NGX_Parameter_Width, guide_width));
  const unsigned int reference_height = get_uint(
      NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
      get_uint(NVSDK_NGX_Parameter_Height, guide_height));
  const unsigned int guide_subrect_width = guide_base_x < guide_width
      ? std::min(reference_width, guide_width - guide_base_x) : 0u;
  const unsigned int guide_subrect_height = guide_base_y < guide_height
      ? std::min(reference_height, guide_height - guide_base_y) : 0u;
  const unsigned int motion_subrect_width = motion_base_x < motion_width
      ? std::min(reference_width, motion_width - motion_base_x) : 0u;
  const unsigned int motion_subrect_height = motion_base_y < motion_height
      ? std::min(reference_height, motion_height - motion_base_y) : 0u;

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
    published_depth = depth_.PublishClone(device.Get(), command_list,
                                          depth, "depth_clone");
    published_motion = motion_.PublishClone(device.Get(), command_list,
                                            motion, "motion_clone");
    if (published_depth == nullptr || published_motion == nullptr) {
      state_ = {};
      state_.clone_resources = clone_resources_;
      state_.capture_serial = capture_serial;
      return;
    }
  } else {
    published_depth = depth_.PublishDirect(depth, retirement_fence_hint);
    published_motion = motion_.PublishDirect(motion, retirement_fence_hint);
  }

  state_.depth = published_depth;
  state_.motion = published_motion;
  state_.guide_width = guide_width;
  state_.guide_height = guide_height;
  state_.motion_width = motion_width;
  state_.motion_height = motion_height;
  state_.guide_base_x = guide_base_x;
  state_.guide_base_y = guide_base_y;
  state_.motion_base_x = motion_base_x;
  state_.motion_base_y = motion_base_y;
  state_.guide_subrect_width = guide_subrect_width;
  state_.guide_subrect_height = guide_subrect_height;
  state_.motion_subrect_width = motion_subrect_width;
  state_.motion_subrect_height = motion_subrect_height;
  state_.reference_width = reference_width;
  state_.reference_height = reference_height;
  state_.mv_scale_x = mv_scale_x;
  state_.mv_scale_y = mv_scale_y;
  state_.depth_inverted =
      (create_flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
  state_.clone_resources = clone_resources_;
  state_.valid = true;
  state_.capture_serial = capture_serial;
}

void DlssGuides::CaptureOutput(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Parameter* params,
    NVSDK_NGX_Feature feature_id,
    uint64_t retirement_fence_hint) {
  DLSSNR_PROFILE_RESOURCE_L2("DlssGuides::CaptureOutput");
  std::lock_guard<std::mutex> lock(lock_);

  state_.dlss_output_color = nullptr;
  state_.output_width = 0;
  state_.output_height = 0;
  if (!IsUpscalerFeature(feature_id) ||
      command_list == nullptr || params == nullptr || !state_.valid) {
    return;
  }

  ID3D12Resource* output =
      GetNgxResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
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

    published_output = output_.PublishClone(device.Get(), command_list, output,
                                            "dlss_output_clone");
    if (published_output == nullptr) return;
  } else {
    published_output = output_.PublishDirect(output, retirement_fence_hint,
                                             command_list);
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


DlssGuideState DlssGuides::GetState(bool invert_fallback_depth) const {
  std::lock_guard<std::mutex> lock(lock_);
  DlssGuideState result = state_;
  if (result.depth == nullptr && fallback_depth_ != nullptr) {
    result.depth = fallback_depth_.Get();
    result.guide_width = fallback_depth_width_;
    result.guide_height = fallback_depth_height_;
    result.guide_base_x = 0;
    result.guide_base_y = 0;
    result.guide_subrect_width = fallback_depth_width_;
    result.guide_subrect_height = fallback_depth_height_;
    result.depth_inverted = invert_fallback_depth;
    result.fallback_depth = true;
    result.valid = true;
  }
  return result;
}

void DlssGuides::SetFallbackDepth(ID3D12Resource* resource) {
  DLSSNR_PROFILE_RESOURCE_L2("DlssGuides::SetFallbackDepth");
  std::lock_guard<std::mutex> lock(lock_);
  if (fallback_depth_.Get() == resource) return;

  fallback_depth_.Reset();
  fallback_depth_width_ = 0;
  fallback_depth_height_ = 0;
  if (resource != nullptr) {
    fallback_depth_ = BorrowedRef::AddRef(resource);
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    fallback_depth_width_ = static_cast<unsigned int>(desc.Width);
    fallback_depth_height_ = desc.Height;
  }
}

void DlssGuides::Clear() {
  DLSSNR_PROFILE_RESOURCE_L1("DlssGuides::Clear");
  std::lock_guard<std::mutex> lock(lock_);

  depth_.Reset();
  motion_.Reset();
  output_.Reset();
  fallback_depth_.Reset();
  fallback_depth_width_ = 0;
  fallback_depth_height_ = 0;
  clone_resources_ = false;
  requested_clone_resources_.store(false, std::memory_order_release);

  // Deliberately no drain here. Force-releasing the queue is the teardown
  // boundary's call, not this object's: the queue is shared, and only the
  // caller that waited for the final fence knows the GPU is done with it.
  state_ = {};
}

}  // namespace reshade_dlssnr

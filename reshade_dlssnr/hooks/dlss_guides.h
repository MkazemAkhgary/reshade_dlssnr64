// Captures and owns DLSS SuperSampling depth/motion/color guide resources.
#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "gpu/gpu_resources.h"

namespace reshade_dlssnr {

// Both upscalers produce the image the addon grades and both consume depth
// and motion vectors, so either one can supply guides.
inline bool IsUpscalerFeature(NVSDK_NGX_Feature feature_id) {
  return feature_id == NVSDK_NGX_Feature_SuperSampling ||
         feature_id == NVSDK_NGX_Feature_RayReconstruction;
}

struct DlssGuideState {
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  // Current DLSS output resource or its optional clone. Used by Post-DLSS
  // HDR metering/Exposure Mask; nullptr unless captured for this evaluation.
  ID3D12Resource* dlss_output_color = nullptr;
  unsigned int guide_width = 0;
  unsigned int guide_height = 0;
  unsigned int motion_width = 0;
  unsigned int motion_height = 0;
  // Active NGX guide rectangles within the underlying resource textures.
  unsigned int guide_base_x = 0;
  unsigned int guide_base_y = 0;
  unsigned int motion_base_x = 0;
  unsigned int motion_base_y = 0;
  unsigned int guide_subrect_width = 0;
  unsigned int guide_subrect_height = 0;
  unsigned int motion_subrect_width = 0;
  unsigned int motion_subrect_height = 0;
  unsigned int reference_width = 0;
  unsigned int reference_height = 0;
  unsigned int output_width = 0;
  unsigned int output_height = 0;
  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;
  bool depth_inverted = false;
  bool fallback_depth = false;
  bool synthetic_motion = false;
  bool clone_resources = false;
  bool valid = false;
  uint64_t capture_serial = 0;
};

class DlssGuides {
 public:
  static DlssGuides& Get();

  // UI/settings code publishes the requested mode atomically. The hook latches
  // it in CaptureGuides at the next NGX evaluation, then uses that same mode
  // through the matching post-evaluation output capture.
  void RequestCloneResources(bool enabled) {
    requested_clone_resources_.store(enabled, std::memory_order_release);
  }

  // Called before the real NGX evaluation with this evaluation's current
  // depth/motion inputs. Mode changes are applied only at this boundary.
  //
  // Resources retired by this call are tagged eagerly with
  // `retirement_fence_hint` -- the release queue's most recently signaled
  // fence value at the time of the call -- rather than waiting for the
  // next Present to tag them. This keeps the addon from holding the game's
  // D3D12 resources indefinitely if Present stops running (debugger stall,
  // profiler pause) while EvaluateFeature keeps firing.
  void CaptureGuides(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Parameter* params,
      NVSDK_NGX_Feature feature_id,
      uint64_t retirement_fence_hint = 0);

  // Called after a successful evaluation when a Post-DLSS pass needs the
  // just-produced output. The mode latched by CaptureGuides is used for the
  // matching evaluation. `retirement_fence_hint` is forwarded to any
  // resources retired by this call; see CaptureGuides for why it exists.
  void CaptureOutput(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Parameter* params,
      NVSDK_NGX_Feature feature_id,
      uint64_t retirement_fence_hint = 0);

  DlssGuideState GetState(bool invert_fallback_depth = false) const;
  void SetFallbackDepth(ID3D12Resource* resource);
  void Clear();

 private:
  DlssGuides() = default;
  ~DlssGuides() = default;

  DlssGuides(const DlssGuides&) = delete;
  DlssGuides& operator=(const DlssGuides&) = delete;

  // Guards every member below. Retirement happens while this is held, so today
  // the order is g_direct_mutex -> lock_ -> ResourceRetireQueue. That works
  // because the queue takes only its own mutex and calls nothing out; taking
  // this lock from inside a queue lock would deadlock. Reorder or merge these
  // locks freely - just keep the queue non-reentrant, and re-check this if you
  // change which lock is taken first.
  mutable std::mutex lock_;

  // Clone allocations are reused while cloning stays enabled. The direct
  // references keep game objects alive until the queue-ordered retirement
  // fence; BorrowedRef owns exactly one counted reference on each.
  OwnedTexture depth_clone_;
  OwnedTexture motion_clone_;
  OwnedTexture dlss_output_clone_;
  BorrowedRef depth_direct_;
  BorrowedRef motion_direct_;
  BorrowedRef dlss_output_direct_;
  BorrowedRef fallback_depth_;
  unsigned int fallback_depth_width_ = 0;
  unsigned int fallback_depth_height_ = 0;
  std::atomic_bool requested_clone_resources_{false};
  bool clone_resources_ = false;
  uint64_t capture_serial_ = 0;

  DlssGuideState state_;

  DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT format) const;
  bool IsTypelessFormat(DXGI_FORMAT format) const;
  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);
};

}  // namespace reshade_dlssnr

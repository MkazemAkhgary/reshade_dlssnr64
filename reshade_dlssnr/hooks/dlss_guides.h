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

inline ID3D12Resource* GetNgxResource(
    const NVSDK_NGX_Parameter* params,
    const char* primary_name,
    const char* alternate_name = nullptr) {
  if (params == nullptr || primary_name == nullptr) return nullptr;
  ID3D12Resource* resource = nullptr;
  if (params->Get(primary_name, &resource) == NVSDK_NGX_Result_Success &&
      resource != nullptr) {
    return resource;
  }
  if (alternate_name != nullptr) {
    resource = nullptr;
    if (params->Get(alternate_name, &resource) == NVSDK_NGX_Result_Success &&
        resource != nullptr) {
      return resource;
    }
  }
  void* untyped = nullptr;
  if (params->Get(primary_name, &untyped) == NVSDK_NGX_Result_Success &&
      untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }
  if (alternate_name != nullptr) {
    untyped = nullptr;
    if (params->Get(alternate_name, &untyped) == NVSDK_NGX_Result_Success &&
        untyped != nullptr) {
      return static_cast<ID3D12Resource*>(untyped);
    }
  }
  return nullptr;
}

// One guide resource: either an addon-owned clone or a direct reference to
// the game's resource, never both.
class GuideSlot {
 public:
  void Reset(uint64_t fence_hint = 0);
  void ReleaseDirect(uint64_t fence_hint = 0);

  [[nodiscard]] ID3D12Resource* PublishClone(ID3D12Device* device,
                                              ID3D12GraphicsCommandList* list,
                                              ID3D12Resource* source,
                                              const char* debug_name);

  [[nodiscard]] ID3D12Resource* PublishDirect(
      ID3D12Resource* source, uint64_t fence_hint,
      ID3D12GraphicsCommandList* transition_list = nullptr);

 private:
  OwnedTexture clone_;
  BorrowedRef direct_;
};

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

  GuideSlot depth_;
  GuideSlot motion_;
  GuideSlot output_;
  BorrowedRef fallback_depth_;
  unsigned int fallback_depth_width_ = 0;
  unsigned int fallback_depth_height_ = 0;
  std::atomic_bool requested_clone_resources_{false};
  bool clone_resources_ = false;
  uint64_t capture_serial_ = 0;

  DlssGuideState state_;

};

}  // namespace reshade_dlssnr

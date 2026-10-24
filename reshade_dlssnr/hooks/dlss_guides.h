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
  void RetireGameResource(ID3D12Resource* resource);
  // Bind all resources retired before this present to a queue-ordered fence.
  // ReleaseRetiredResourcesCompleted() later frees only the completed prefix.
  void TagRetiredResources(uint64_t fence_value);
  size_t ReleaseRetiredResourcesCompleted(uint64_t completed_fence_value);
  void Clear();

  // Queue depths, for the frame-boundary diagnostic in AddonController.
  size_t RetiredResourceCount() const;
  size_t RetiredGameResourceCount() const;

 private:
  struct RetiredResource {
    ID3D12Resource* resource = nullptr;
    uint64_t fence_value = 0;
  };

  void ReleaseRetiredResourcesLocked();
  void ReleaseRetiredGameResourcesLocked();
  size_t ReleaseCompletedLocked(std::vector<RetiredResource>& resources,
                               uint64_t completed_fence_value);

 private:
  DlssGuides() = default;
  ~DlssGuides() = default;

  DlssGuides(const DlssGuides&) = delete;
  DlssGuides& operator=(const DlssGuides&) = delete;

  mutable std::mutex lock_;

  // Clone allocations are reused while cloning stays enabled. Direct-resource
  // ComPtrs keep game objects alive until the queue-ordered retirement fence.
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> dlss_output_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> depth_direct_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_direct_;
  Microsoft::WRL::ComPtr<ID3D12Resource> dlss_output_direct_;
  Microsoft::WRL::ComPtr<ID3D12Resource> fallback_depth_;
  unsigned int fallback_depth_width_ = 0;
  unsigned int fallback_depth_height_ = 0;
  std::atomic_bool requested_clone_resources_{false};
  bool clone_resources_ = false;
  uint64_t capture_serial_ = 0;

  unsigned int depth_width_ = 0;
  unsigned int depth_height_ = 0;
  DXGI_FORMAT depth_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int motion_width_ = 0;
  unsigned int motion_height_ = 0;
  DXGI_FORMAT motion_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int dlss_output_width_ = 0;
  unsigned int dlss_output_height_ = 0;
  DXGI_FORMAT dlss_output_format_ = DXGI_FORMAT_UNKNOWN;

  std::vector<RetiredResource> retired_resources_;
  std::vector<RetiredResource> retired_game_resources_;

  DlssGuideState state_;

  void RetireResource(Microsoft::WRL::ComPtr<ID3D12Resource>& resource);

  ID3D12Resource* CreateGuideClone(
      ID3D12Device* device,
      ID3D12Resource* source,
      unsigned int* out_width,
      unsigned int* out_height,
      DXGI_FORMAT* out_format);

  DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT format) const;
  bool IsTypelessFormat(DXGI_FORMAT format) const;
  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);
};

}  // namespace reshade_dlssnr

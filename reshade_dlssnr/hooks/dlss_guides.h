// Captures and owns DLSS SuperSampling depth/motion/color guide resources.
#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

namespace reshade_dlssnr {

struct DlssGuideState {
  ID3D12Resource* depth = nullptr;
  ID3D12Resource* motion = nullptr;
  unsigned int guide_width = 0;
  unsigned int guide_height = 0;
  float mv_scale_x = 1.0f;
  float mv_scale_y = 1.0f;
  bool depth_inverted = false;
  bool valid = false;
};

class DlssGuides {
 public:
  static DlssGuides& Get();

  void Capture(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Parameter* params,
      NVSDK_NGX_Feature feature_id);

  void SetSelectedPassHandle(uint64_t handle);
  void ClearSelectedPass();
  void CaptureSelectedPassIfNeeded(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* selected_pass_source);

  DlssGuideState GetState() const;
  void ReleaseRetiredResources();
  void RetireGameResource(ID3D12Resource* resource);
  void ReleaseRetiredGameResources();
  void Clear();

 private:
  void ReleaseRetiredResourcesLocked();
  void ReleaseRetiredGameResourcesLocked();

 private:
  DlssGuides() = default;
  ~DlssGuides() = default;

  DlssGuides(const DlssGuides&) = delete;
  DlssGuides& operator=(const DlssGuides&) = delete;

  mutable std::mutex lock_;

  Microsoft::WRL::ComPtr<ID3D12Resource> depth_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> motion_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> pre_ui_clone_;
  Microsoft::WRL::ComPtr<ID3D12Resource> selected_pass_clone_;

  unsigned int depth_width_ = 0;
  unsigned int depth_height_ = 0;
  DXGI_FORMAT depth_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int motion_width_ = 0;
  unsigned int motion_height_ = 0;
  DXGI_FORMAT motion_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int pre_ui_width_ = 0;
  unsigned int pre_ui_height_ = 0;
  DXGI_FORMAT pre_ui_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int selected_pass_width_ = 0;
  unsigned int selected_pass_height_ = 0;
  DXGI_FORMAT selected_pass_format_ = DXGI_FORMAT_UNKNOWN;

  uint64_t selected_pass_handle_ = 0;
  bool has_selected_pass_handle_ = false;

  std::vector<ID3D12Resource*> retired_resources_;
  std::vector<ID3D12Resource*> retired_game_resources_;

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
  D3D12_RESOURCE_STATES InferResourceState(ID3D12Resource* resource) const;
  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);
};

}  // namespace reshade_dlssnr

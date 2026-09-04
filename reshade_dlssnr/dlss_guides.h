#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

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

  DlssGuideState Get() const;
  void Clear();

 private:
  DlssGuides() = default;
  ~DlssGuides() = default;

  DlssGuides(const DlssGuides&) = delete;
  DlssGuides& operator=(const DlssGuides&) = delete;

  mutable CRITICAL_SECTION lock_;

  ID3D12Resource* depth_clone_ = nullptr;
  ID3D12Resource* motion_clone_ = nullptr;

  unsigned int depth_width_ = 0;
  unsigned int depth_height_ = 0;
  DXGI_FORMAT depth_format_ = DXGI_FORMAT_UNKNOWN;

  unsigned int motion_width_ = 0;
  unsigned int motion_height_ = 0;
  DXGI_FORMAT motion_format_ = DXGI_FORMAT_UNKNOWN;

  DlssGuideState state_;

  ID3D12Resource* CreateGuideClone(
      ID3D12Device* device,
      ID3D12Resource* source,
      unsigned int* out_width,
      unsigned int* out_height,
      DXGI_FORMAT* out_format);

  DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT format) const;
  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);
};

}  // namespace reshade_dlssnr

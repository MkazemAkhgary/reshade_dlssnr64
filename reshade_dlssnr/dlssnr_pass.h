#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <memory>

#include "codec.h"
#include "forwarder.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrPass {
 public:
  static DlssNrPass& Get();

  bool IsReady() const;
  const char* GetLastError() const;

  void OnEvaluateAfterUpscale(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Parameter* params,
      NVSDK_NGX_Feature feature_id,
      ID3D12CommandQueue* queue = nullptr);

  void Shutdown();

 private:
  DlssNrPass() = default;
  ~DlssNrPass() = default;

  DlssNrPass(const DlssNrPass&) = delete;
  DlssNrPass& operator=(const DlssNrPass&) = delete;

  bool EnsureForwarder();
  bool EnsureCapabilityParams(ID3D12Device* device);
  bool EnsureFeature(ID3D12Device* device, ID3D12GraphicsCommandList* command_list, unsigned int width, unsigned int height);
  void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
  void SetExtras();
  void RetireResources(ID3D12Device* device);

  ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height);
  void Barrier(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
  DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT format) const;
  ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* command_list, ID3D12Resource* source, ID3D12Resource** clone);
  ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source);
  bool FormatCanHoldLinearHdr(DXGI_FORMAT format) const;
  ID3D12Resource* GetResource(const NVSDK_NGX_Parameter* params, const char* name_a, const char* name_b);

  ForwarderApi forwarder_;
  NVSDK_NGX_Parameter* capability_params_ = nullptr;
  void* feature_ = nullptr;
  bool float_slot_known_ = false;
  int float_slot_ = 1;
  bool failed_ = false;
  const char* reason_ = "";

  std::unique_ptr<DlssNrCodec> codec_;

  ID3D12Resource* color_copy_ = nullptr;
  ID3D12Resource* hdr_copy_ = nullptr;
  ID3D12Resource* output_ = nullptr;
  ID3D12Resource* color_small_ = nullptr;
  ID3D12Resource* depth_clone_ = nullptr;
  ID3D12Resource* motion_clone_ = nullptr;

  unsigned int width_ = 0;
  unsigned int height_ = 0;
  unsigned int work_width_ = 0;
  unsigned int work_height_ = 0;
  bool reset_ = true;
  bool built_tuning_dirty_ = true;

  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
  unsigned int built_work_width_ = 0;
  unsigned int built_work_height_ = 0;
  DXGI_FORMAT built_format_ = DXGI_FORMAT_UNKNOWN;
  bool built_passthrough_ = false;
  int built_preset_ = -1;
  int built_style_ = -1;
  float built_intensity_ = -1.0f;
  float built_local_structure_ = -1.0f;
  float built_local_tone_ = -1.0f;
  float built_skin_structure_ = -1.0f;
  bool built_auto_mask_ = false;
};

}  // namespace reshade_dlssnr

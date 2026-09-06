#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "codec.h"

namespace reshade_dlssnr {

class NgxParameterBlock {
 public:
  bool Initialize();
  void Shutdown();

  bool IsReady() const { return params_ != nullptr; }
  NVSDK_NGX_Parameter* Get() const { return params_; }

  void SetUInt(const char* name, unsigned int value);
  void SetInt(const char* name, int value);
  void SetFloat(const char* name, float value);
  void SetResource(const char* name, ID3D12Resource* value);
  void SetPointer(const char* name, void* value);

 private:
  void ProbeFloatSlot();

  HMODULE core_module_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
  int float_slot_ = 1;
  bool float_slot_known_ = false;
};

class DlssNrDirect {
 public:
  static DlssNrDirect& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  void SetDebugSelectedResource(uint64_t handle);
  void ClearSelectedPass();
  void SetPendingSelectedPass(ID3D12Resource* resource);

  void Shutdown();

  bool IsReady() const;
  const char* GetLastError() const;

 private:
  DlssNrDirect() = default;
  ~DlssNrDirect() = default;

  DlssNrDirect(const DlssNrDirect&) = delete;
  DlssNrDirect& operator=(const DlssNrDirect&) = delete;

  bool EnsureModelAndPatch();
  bool EnsureInitialized(ID3D12Device* device);
  bool EnsureParameterBlock();
  bool EnsureFeature(ID3D12GraphicsCommandList* command_list, unsigned int width, unsigned int height);
  bool EnsureCommandInfrastructure(ID3D12Device* device);
  void WaitGpu();
  void Barrier(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  HMODULE model_module_ = nullptr;
  NgxParameterBlock parameter_block_;

  using PFN_InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
  using PFN_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
  using PFN_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
  using PFN_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

  PFN_InitExt init_ext_ = nullptr;
  PFN_CreateFeature create_feature_ = nullptr;
  PFN_EvaluateFeature evaluate_feature_ = nullptr;
  PFN_ReleaseFeature release_feature_ = nullptr;

  bool initialized_ = false;
  bool failed_ = false;
  bool first_evaluate_ = true;
  const char* reason_ = "";

  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
  float built_working_scale_ = -1.0f;
  unsigned int built_preset_ = 0xFFFFFFFFu;
  float built_intensity_ = -1.0f;
  float built_local_structure_ = -1.0f;
  float built_local_tone_ = -1.0f;
  float built_skin_structure_ = -1.0f;
  bool built_auto_mask_ = false;
  bool built_ui_correction_ = false;

  ID3D12Resource* output_ = nullptr;
  unsigned int output_width_ = 0;
  unsigned int output_height_ = 0;

  ID3D12Resource* ui_mask_ = nullptr;
  unsigned int ui_mask_width_ = 0;
  unsigned int ui_mask_height_ = 0;
  DXGI_FORMAT ui_mask_format_ = DXGI_FORMAT_UNKNOWN;

  ID3D12Resource* composite_output_ = nullptr;
  unsigned int composite_output_width_ = 0;
  unsigned int composite_output_height_ = 0;
  DXGI_FORMAT composite_output_format_ = DXGI_FORMAT_UNKNOWN;

  ID3D12Resource* working_input_ = nullptr;
  ID3D12Resource* working_output_ = nullptr;
  unsigned int working_resource_width_ = 0;
  unsigned int working_resource_height_ = 0;
  DXGI_FORMAT working_resource_format_ = DXGI_FORMAT_UNKNOWN;

  const PassSettings* current_pass_ = nullptr;

  ID3D12CommandQueue* command_queue_ = nullptr;
  ID3D12CommandAllocator* command_allocator_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = INVALID_HANDLE_VALUE;
  UINT64 fence_value_ = 0;

  std::unique_ptr<DlssNrCodec> debug_codec_;

  uint64_t debug_selected_resource_handle_ = 0;
  ID3D12Resource* pending_selected_pass_ = nullptr;
};

}  // namespace reshade_dlssnr

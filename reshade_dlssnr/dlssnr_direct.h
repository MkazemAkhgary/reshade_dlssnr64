#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <string>
#include <unordered_map>

namespace reshade_dlssnr {

class SimpleParams : public NVSDK_NGX_Parameter {
 public:
  void Set(const char* name, unsigned long long value) override { ull_[name] = value; }
  void Set(const char* name, float value) override { f_[name] = value; }
  void Set(const char* name, double value) override { d_[name] = value; }
  void Set(const char* name, unsigned int value) override { ui_[name] = value; }
  void Set(const char* name, int value) override { i_[name] = value; }
  void Set(const char* name, ID3D11Resource* value) override { ptr_[name] = value; }
  void Set(const char* name, ID3D12Resource* value) override { ptr_[name] = value; }
  void Set(const char* name, void* value) override { ptr_[name] = value; }

  NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const override;
  NVSDK_NGX_Result Get(const char* name, float* value) const override;
  NVSDK_NGX_Result Get(const char* name, double* value) const override;
  NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override;
  NVSDK_NGX_Result Get(const char* name, int* value) const override;
  NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const override;
  NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override;
  NVSDK_NGX_Result Get(const char* name, void** value) const override;

  void Reset() override;

 private:
  std::unordered_map<std::string, unsigned long long> ull_;
  std::unordered_map<std::string, float> f_;
  std::unordered_map<std::string, double> d_;
  std::unordered_map<std::string, unsigned int> ui_;
  std::unordered_map<std::string, int> i_;
  std::unordered_map<std::string, void*> ptr_;
};

class DlssNrDirect {
 public:
  static DlssNrDirect& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

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
  bool EnsureFeature(ID3D12GraphicsCommandList* command_list, unsigned int width, unsigned int height);
  bool EnsureCommandInfrastructure(ID3D12Device* device);
  void WaitGpu();
  void Barrier(ID3D12GraphicsCommandList* command_list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

  HMODULE model_module_ = nullptr;

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
  const char* reason_ = "";

  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;

  ID3D12Resource* output_ = nullptr;
  unsigned int output_width_ = 0;
  unsigned int output_height_ = 0;

  ID3D12CommandQueue* command_queue_ = nullptr;
  ID3D12CommandAllocator* command_allocator_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = INVALID_HANDLE_VALUE;
  UINT64 fence_value_ = 0;
};

}  // namespace reshade_dlssnr

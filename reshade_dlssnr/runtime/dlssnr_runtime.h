// Shared model loading, caller patch, NGX init, and parameter block ownership.
#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <filesystem>
#include <mutex>

#include "ngx_parameter_block.h"

namespace reshade_dlssnr {

class DlssNrRuntime {
 public:
  bool Initialize(ID3D12Device* device);
  void Shutdown();

  NgxParameterBlock& Parameters() { return parameter_block_; }
  const NgxParameterBlock& Parameters() const { return parameter_block_; }

  NVSDK_NGX_Result CreateFeature(
      ID3D12GraphicsCommandList* command_list,
      NVSDK_NGX_Feature feature_id,
      NVSDK_NGX_Parameter* params,
      NVSDK_NGX_Handle** out_handle);

  NVSDK_NGX_Result EvaluateFeature(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Handle* feature_handle,
      const NVSDK_NGX_Parameter* params,
      void* callback);

  NVSDK_NGX_Result ReleaseFeature(NVSDK_NGX_Handle* feature_handle);

  bool IsInitialized() const { return initialized_; }
  const char* GetLastError() const { return reason_; }

 private:
  bool EnsureModelAndPatch();
  bool EnsureInitialized(ID3D12Device* device);

  HMODULE model_module_ = nullptr;
  NgxParameterBlock parameter_block_;

  using PFN_InitExt = NVSDK_NGX_Result(NVSDK_CONV*)(
      unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version,
      const NVSDK_NGX_Parameter*);
  using PFN_CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
      ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
      NVSDK_NGX_Handle**);
  using PFN_EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(
      ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
      const NVSDK_NGX_Parameter*, void*);
  using PFN_ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

  PFN_InitExt init_ext_ = nullptr;
  PFN_CreateFeature create_feature_ = nullptr;
  PFN_EvaluateFeature evaluate_feature_ = nullptr;
  PFN_ReleaseFeature release_feature_ = nullptr;

  bool initialized_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

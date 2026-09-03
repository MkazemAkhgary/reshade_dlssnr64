#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <detours.h>

#include "../../renodx/src/utils/directx.hpp"

#include "dlssnr_pass.h"

namespace reshade_dlssnr {

namespace {

// RenoDX-style native export hooking for nvngx_dlss.dll. No proxy DLL,
// no MinHook prologue patching.
struct NvngxHook {
  static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature;
  static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_NVSDK_NGX_D3D12_CreateFeature;

  static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_EvaluateFeature(
      ID3D12GraphicsCommandList* command_list,
      const NVSDK_NGX_Handle* feature_handle,
      const NVSDK_NGX_Parameter* parameters,
      PFN_NVSDK_NGX_ProgressCallback callback) {
    renodx::utils::directx::NativeFromReShadeProxy(&command_list);

    NVSDK_NGX_Parameter* native_parameters = const_cast<NVSDK_NGX_Parameter*>(parameters);
    NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(
        command_list, feature_handle, native_parameters, callback);

    if (result == NVSDK_NGX_Result_Success && feature_handle != nullptr) {
      DlssNrPass::Get().OnEvaluateAfterUpscale(
          command_list,
          parameters,
          NVSDK_NGX_Feature_SuperSampling);
    }

    return result;
  }
};

}  // namespace

}  // namespace reshade_dlssnr

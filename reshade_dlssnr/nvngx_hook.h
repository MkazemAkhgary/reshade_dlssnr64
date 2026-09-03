#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <detours.h>

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
    command_list = static_cast<ID3D12GraphicsCommandList*>(command_list);
    auto* native_command_list = command_list;
    renodx_utils_directx_NativeFromReShadeProxy(&native_command_list);

    NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(native_command_list, feature_handle, parameters, callback);

    if (result == NVSDK_NGX_Result_Success && feature_handle != nullptr) {
      // Feature detection is intentionally conservative: the handle id alone
      // is not enough, so the caller supplies the feature id through the
      // addon hook bridge.
      DlssNrPass::Get().OnEvaluateAfterUpscale(
          native_command_list,
          const_cast<NVSDK_NGX_Parameter*>(parameters),
          NVSDK_NGX_Feature_SuperSampling);
    }

    return result;
  }
};

}  // namespace

}  // namespace reshade_dlssnr

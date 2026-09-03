#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <cassert>
#include <tuple>
#include <vector>

#include <detours.h>

#include "../../renodx/src/utils/directx.hpp"
#include "../../renodx/src/utils/log.hpp"
#include "../../renodx/src/utils/vtable.hpp"

#include "d3d12_hooks.h"

namespace reshade_dlssnr::renodx_compat {

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  D3D12Hooks::Get().HookCommandList(command_list);

  return real_NVSDK_NGX_D3D12_EvaluateFeature(
      command_list, feature_handle, parameters, callback);
}

using HookItem = std::tuple<const char*, void**, void*>;

static const std::vector<HookItem> DLSS_HOOKS = {
    {"NVSDK_NGX_D3D12_EvaluateFeature", (void**)&real_NVSDK_NGX_D3D12_EvaluateFeature, (void*)&hooked_NVSDK_NGX_D3D12_EvaluateFeature},
};

}  // namespace reshade_dlssnr::renodx_compat

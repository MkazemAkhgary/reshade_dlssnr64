// RenoDX-compatible NVSDK_NGX_D3D12 hook bridge for guide capture.
#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <cassert>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <detours.h>

#include "d3d12_hooks.h"
#include "dlss_guides.h"

namespace reshade_dlssnr::renodx_compat {

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_NVSDK_NGX_D3D12_CreateFeature = nullptr;

static inline std::unordered_map<unsigned int, NVSDK_NGX_Feature> g_handle_to_feature;
static inline std::mutex g_handle_to_feature_mutex;

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* command_list,
    NVSDK_NGX_Feature feature_id,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** out_handle) {
  NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_CreateFeature(
      command_list, feature_id, parameters, out_handle);

  if (result == NVSDK_NGX_Result_Success && out_handle != nullptr && *out_handle != nullptr) {
    std::lock_guard<std::mutex> lock(g_handle_to_feature_mutex);
    g_handle_to_feature[(*out_handle)->Id] = feature_id;
  }

  return result;
}

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  D3D12Hooks::Get().SetTrackingEnabled(false);

  NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(
      command_list, feature_handle, parameters, callback);

  if (result == NVSDK_NGX_Result_Success && feature_handle != nullptr && parameters != nullptr) {
    NVSDK_NGX_Feature feature_id = NVSDK_NGX_Feature_Reserved0;

    {
      std::lock_guard<std::mutex> lock(g_handle_to_feature_mutex);
      auto it = g_handle_to_feature.find(feature_handle->Id);
      if (it != g_handle_to_feature.end()) {
        feature_id = it->second;
      }
    }

    if (feature_id == NVSDK_NGX_Feature_SuperSampling) {
      DlssGuides::Get().Capture(command_list, parameters, feature_id);
    }
  }

  D3D12Hooks::Get().RestoreCommandList(command_list);
  D3D12Hooks::Get().SetTrackingEnabled(true);

  return result;
}

using HookItem = std::tuple<const char*, void**, void*>;

static const std::vector<HookItem> DLSS_HOOKS = {
    {"NVSDK_NGX_D3D12_CreateFeature", (void**)&real_NVSDK_NGX_D3D12_CreateFeature, (void*)&hooked_NVSDK_NGX_D3D12_CreateFeature},
    {"NVSDK_NGX_D3D12_EvaluateFeature", (void**)&real_NVSDK_NGX_D3D12_EvaluateFeature, (void*)&hooked_NVSDK_NGX_D3D12_EvaluateFeature},
};

namespace local_vtable {

static bool Hook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return false;

  bool any_hooked = false;
  bool transaction_started = false;

  for (const auto& [name, real, hook] : hooks) {
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr) continue;

    *real = reinterpret_cast<void*>(proc);

    if (!transaction_started) {
      if (DetourTransactionBegin() != NO_ERROR) continue;
      if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
        DetourTransactionAbort();
        continue;
      }
      transaction_started = true;
    }

    if (DetourAttach(reinterpret_cast<void**>(real), hook) == NO_ERROR) {
      any_hooked = true;
    } else {
      DetourTransactionAbort();
      transaction_started = false;
    }
  }

  if (transaction_started) {
    if (DetourTransactionCommit() != NO_ERROR) {
      DetourTransactionAbort();
      any_hooked = false;
    }
  }

  return any_hooked;
}

static void Unhook(HMODULE module, const std::vector<HookItem>& hooks) {
  if (module == nullptr) return;

  for (const auto& [name, real, hook] : hooks) {
    if (*real == nullptr || hook == nullptr) continue;

    if (DetourTransactionBegin() != NO_ERROR) continue;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) {
      DetourTransactionAbort();
      continue;
    }

    if (DetourDetach(reinterpret_cast<void**>(real), hook) != NO_ERROR) {
      DetourTransactionAbort();
      continue;
    }

    if (DetourTransactionCommit() == NO_ERROR) {
      *real = nullptr;
    } else {
      DetourTransactionAbort();
    }
  }
}

}  // namespace local_vtable

}  // namespace reshade_dlssnr::renodx_compat

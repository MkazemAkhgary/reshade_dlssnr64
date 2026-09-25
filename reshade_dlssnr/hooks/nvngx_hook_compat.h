// RenoDX-compatible NVSDK_NGX_D3D12 hook bridge for guide capture.
#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <detours.h>

#include <reshade.hpp>

#include "addon_controller.h"
#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "local_vtable.h"
#include "proxy_unwrap.h"
#include "settings.h"

namespace reshade_dlssnr::ngx_hook_compat {

// Installing the D3D12 hooks allocates a command allocator and a command
// list, so a retry that never succeeds must not run forever.
constexpr int kMaxHookInstallRetries = 120;

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_NVSDK_NGX_D3D12_CreateFeature = nullptr;

static inline std::unordered_map<unsigned int, NVSDK_NGX_Feature> g_handle_to_feature;
static inline std::mutex g_handle_to_feature_mutex;

// Logged once: whether the game's command list had to be resolved. ReShade
// always wraps, so on any host this should say "unwrapped" -- if it ever says
// otherwise, the resolution stopped working and every restore is a no-op
// again, silently.
static void LogCommandListProxyOnce(bool was_proxy) {
  static bool logged = false;
  if (logged) return;
  logged = true;

  reshade::log::message(
      reshade::log::level::info,
      was_proxy
          ? "Command list resolved to the interface our D3D12 hooks recorded"
          : "Command list is already the interface our D3D12 hooks recorded");
}

static void LogOnce(bool& flag, reshade::log::level level, const char* message) {
  if (flag) return;
  flag = true;
  reshade::log::message(level, message);
}

// The vtable hooks are installed at device init, but they are what lets us
// put the game's command list state back after a Post-DLSS pass, so try again
// here if the first attempt did not take.
static bool EnsureHooks(ID3D12GraphicsCommandList* command_list) {
  if (D3D12Hooks::Get().Installed()) return true;
  if (command_list == nullptr) return false;

  static int retries = 0;
  if (retries >= kMaxHookInstallRetries) return false;
  ++retries;

  ID3D12Device* device = nullptr;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return false;
  }

  const bool installed = D3D12Hooks::Get().InstallEarlyHooks(device);
  device->Release();
  return installed;
}

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

  // Streamline's own evaluate path is what called us, so hand it back exactly
  // the command list it passed in. Everything we do ourselves goes through the
  // native interface below.
  NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(
      command_list, feature_handle, parameters, callback);

  // Whether this hook fires at all, and what NGX returned. Without it, "no
  // Post-DLSS" and "no evaluate ever arrived" look identical -- a distinction
  // that cost a whole debugging round.
  static bool logged_first = false;
  if (!logged_first) {
    logged_first = true;
    char message[128];
    std::snprintf(message, sizeof(message),
                  "NGX evaluate hook fired (result=%d)",
                  static_cast<int>(result));
    reshade::log::message(reshade::log::level::info, message);
  }

  // The command list the game hands us is ReShade's wrapper, and under
  // Streamline 1.x that wrapper sits around an interposer proxy. Neither is
  // the object our vtable hooks were installed on, and the state tracker is
  // keyed by what those hooks see -- so resolve to that chain here, once, and
  // use it for everything we do.
  //
  // Recording through a Streamline proxy is also unsafe in its own right: its
  // ResourceBarrier runs Streamline's registered after-hooks from the middle
  // of its own evaluate. Resolving avoids that too.
  bool is_proxy = false;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native_list =
      proxy_unwrap::NativeChain(command_list, &is_proxy);
  LogCommandListProxyOnce(is_proxy);

  ID3D12GraphicsCommandList* work_list =
      native_list != nullptr ? native_list.Get() : command_list;

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
      if (!EnsureHooks(work_list)) {
        AddonController::Get().SetPostDlssBlocked(
            "Post-DLSS disabled: D3D12 command list hooks are not installed");
      } else {
        DlssGuides::Get().Capture(work_list, parameters, feature_id);

        const Settings& settings = GetSettings();
        if (settings.enabled) {
          bool any_post_dlss = false;
          for (uint32_t i = 0; i < kMaxPasses; ++i) {
            if (settings.pass_enabled[i] &&
                settings.passes[i].injection_stage == InjectionStage::PostDLSS) {
              any_post_dlss = true;
              break;
            }
          }

          AddonController::Get().SetPostDlssBlocked(nullptr);

          if (any_post_dlss) {
            ID3D12Resource* output = nullptr;
            if (parameters->Get(NVSDK_NGX_Parameter_Output, &output) ==
                    NVSDK_NGX_Result_Success &&
                output != nullptr) {
              AddonController::Get().OnPostDlssEvaluate(work_list, output);
            }
          }
        }
      }
    }
  }

  D3D12Hooks::Get().RestoreCommandList(work_list);
  D3D12Hooks::Get().SetTrackingEnabled(true);

  return result;
}

using HookItem = local_vtable::HookItem;

static const std::vector<HookItem> DLSS_HOOKS = {
    {"NVSDK_NGX_D3D12_CreateFeature", (void**)&real_NVSDK_NGX_D3D12_CreateFeature, (void*)&hooked_NVSDK_NGX_D3D12_CreateFeature},
    {"NVSDK_NGX_D3D12_EvaluateFeature", (void**)&real_NVSDK_NGX_D3D12_EvaluateFeature, (void*)&hooked_NVSDK_NGX_D3D12_EvaluateFeature},
};

// Installs the NGX hooks. Safe to call until it succeeds.
static bool TryHookNgx() {
  if (real_NVSDK_NGX_D3D12_EvaluateFeature != nullptr) return true;

  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) {
    // nvngx_dlss.dll is loaded on demand, so load it to get the hook in place
    // ahead of the first CreateFeature -- a feature created before this
    // attaches would never be recognised and nothing would run. Checking
    // GetModuleHandle first means an instance Streamline has already loaded is
    // never shadowed by a second copy.
    nvngx_dlss = LoadLibraryW(L"nvngx_dlss.dll");
  }
  if (nvngx_dlss == nullptr) return false;

  if (!local_vtable::Hook(nvngx_dlss, DLSS_HOOKS)) return false;

  static bool logged = false;
  LogOnce(logged, reshade::log::level::info,
          "Hooked NVSDK_NGX_D3D12_CreateFeature/EvaluateFeature in nvngx_dlss.dll");
  return true;
}

static void UnhookNgx() {
  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) return;

  local_vtable::Unhook(nvngx_dlss, DLSS_HOOKS);
}

}  // namespace reshade_dlssnr::ngx_hook_compat

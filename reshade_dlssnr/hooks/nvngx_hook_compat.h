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

#include <reshade.hpp>

#include "addon_controller.h"
#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "local_vtable.h"
#include "settings.h"

namespace reshade_dlssnr::ngx_hook_compat {

static decltype(&NVSDK_NGX_D3D12_EvaluateFeature) real_NVSDK_NGX_D3D12_EvaluateFeature = nullptr;
static decltype(&NVSDK_NGX_D3D12_CreateFeature) real_NVSDK_NGX_D3D12_CreateFeature = nullptr;

static inline std::unordered_map<unsigned int, NVSDK_NGX_Feature> g_handle_to_feature;
static inline std::mutex g_handle_to_feature_mutex;

// Post-DLSS edits re-enter NGX from inside the game's own EvaluateFeature
// call. When the game goes through NVIDIA Streamline and ReShade isn't the
// one owning sl.interposer's entry point, that re-entry happens in the
// middle of Streamline's dispatch and crashes. ReShade only registers that
// hook when [INSTALL] HookStreamline=1, so the value here is exactly the
// condition we need — read once, since ReShade reads its global config
// only at load.
static bool HookStreamlineEnabled() {
  static const bool enabled = [] {
    bool v = false;
    reshade::get_config_value(nullptr, "INSTALL", "HookStreamline", v);
    return v;
  }();
  return enabled;
}

// Whether the game is loading through Streamline. Cached once: in
// practice the game loads sl.interposer.dll at process start, long before
// its first DLSS evaluation, so a single check on the first call is
// sufficient and keeps the per-frame path off the loader lock.
static bool StreamlineLoaded() {
  static const bool loaded =
      GetModuleHandleW(L"sl.interposer.dll") != nullptr;
  return loaded;
}

// True when running a Post-DLSS pass would re-enter NGX unsafely.
static bool PostDlssUnsafe() {
  return StreamlineLoaded() && !HookStreamlineEnabled();
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

        if (any_post_dlss && PostDlssUnsafe()) {
          // Skip rather than crash. The controller surfaces this in the
          // Status section so the cause is visible without the log.
          AddonController::Get().SetPostDlssBlocked(
              "Post-DLSS disabled: set HookStreamline=1 in ReShade.ini");
        } else {
          AddonController::Get().SetPostDlssBlocked(nullptr);
          if (any_post_dlss) {
            ID3D12Resource* output = nullptr;
            if (parameters->Get(NVSDK_NGX_Parameter_Output, &output) ==
                    NVSDK_NGX_Result_Success &&
                output != nullptr) {
              AddonController::Get().OnPostDlssEvaluate(command_list, output);
            }
          }
        }
      }
    }
  }

  D3D12Hooks::Get().RestoreCommandList(command_list);
  D3D12Hooks::Get().SetTrackingEnabled(true);

  return result;
}

using HookItem = local_vtable::HookItem;

static const std::vector<HookItem> DLSS_HOOKS = {
    {"NVSDK_NGX_D3D12_CreateFeature", (void**)&real_NVSDK_NGX_D3D12_CreateFeature, (void*)&hooked_NVSDK_NGX_D3D12_CreateFeature},
    {"NVSDK_NGX_D3D12_EvaluateFeature", (void**)&real_NVSDK_NGX_D3D12_EvaluateFeature, (void*)&hooked_NVSDK_NGX_D3D12_EvaluateFeature},
};

}  // namespace reshade_dlssnr::ngx_hook_compat

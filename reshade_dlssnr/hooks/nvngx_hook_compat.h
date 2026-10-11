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
#include "state/settings.h"

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

static ID3D12Resource* GetResourceParameter(NVSDK_NGX_Parameter* parameters,
                                                const char* name) {
  if (parameters == nullptr || name == nullptr) return nullptr;
  ID3D12Resource* resource = nullptr;
  if (parameters->Get(name, &resource) == NVSDK_NGX_Result_Success &&
      resource != nullptr) {
    return resource;
  }
  void* untyped = nullptr;
  if (parameters->Get(name, &untyped) == NVSDK_NGX_Result_Success &&
      untyped != nullptr) {
    return static_cast<ID3D12Resource*>(untyped);
  }
  return nullptr;
}

static bool IsSupportedPreDlssColor(ID3D12Resource* color,
                                    NVSDK_NGX_Parameter* parameters) {
  if (color == nullptr || parameters == nullptr) return false;
  const D3D12_RESOURCE_DESC desc = color->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
      desc.SampleDesc.Count != 1 || desc.Width == 0 || desc.Height == 0) {
    return false;
  }

  unsigned int input_width = 0;
  unsigned int input_height = 0;
  unsigned int base_x = 0;
  unsigned int base_y = 0;
  parameters->Get(NVSDK_NGX_Parameter_Width, &input_width);
  parameters->Get(NVSDK_NGX_Parameter_Height, &input_height);
  parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, &base_x);
  parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, &base_y);

  // Writing the whole Color resource is only safe when NGX consumes the whole
  // texture. Never overwrite unrelated pixels in an atlas/subrect allocation.
  return base_x == 0 && base_y == 0 &&
         (input_width == 0 || input_width == desc.Width) &&
         (input_height == 0 || input_height == desc.Height);
}

static NVSDK_NGX_Result NVSDK_CONV hooked_NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* command_list,
    const NVSDK_NGX_Handle* feature_handle,
    NVSDK_NGX_Parameter* parameters,
    PFN_NVSDK_NGX_ProgressCallback callback) {
  D3D12Hooks::Get().SetTrackingEnabled(false);

  // Exposure and Color are captured before NGX evaluates, since the parameter
  // block may be reused or mutated by the driver's implementation.
  ID3D12Resource* exposure_texture =
      GetResourceParameter(parameters, NVSDK_NGX_Parameter_ExposureTexture);

  bool is_proxy = false;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> native_list =
      proxy_unwrap::NativeChain(command_list, &is_proxy);
  LogCommandListProxyOnce(is_proxy);
  ID3D12GraphicsCommandList* work_list =
      native_list != nullptr ? native_list.Get() : command_list;

  NVSDK_NGX_Feature feature_id = NVSDK_NGX_Feature_Reserved0;
  if (feature_handle != nullptr) {
    std::lock_guard<std::mutex> lock(g_handle_to_feature_mutex);
    auto it = g_handle_to_feature.find(feature_handle->Id);
    if (it != g_handle_to_feature.end()) feature_id = it->second;
  }

  const Settings& settings = GetSettings();
  bool any_pass_enabled = false;
  bool any_pre_dlss = false;
  bool any_post_dlss = false;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    any_pass_enabled = true;
    any_pre_dlss |= settings.passes[i].injection_stage == InjectionStage::PreDLSS;
    any_post_dlss |= settings.passes[i].injection_stage == InjectionStage::PostDLSS;
  }

  bool recorded_pass = false;
  bool hooks_available = false;
  bool guides_captured_before_evaluate = false;
  if (feature_id == NVSDK_NGX_Feature_SuperSampling && settings.enabled &&
      any_pass_enabled) {
    hooks_available = EnsureHooks(work_list);
    if (hooks_available) {
      // DLSS has already populated these resources before EvaluateFeature is
      // entered. Capture the current frame before Pre-DLSS so temporal guide
      // inputs are correct for all three injection points.
      DlssGuides::Get().CaptureGuides(work_list, parameters, feature_id);
      guides_captured_before_evaluate = true;
    }
  }

  if (feature_id == NVSDK_NGX_Feature_SuperSampling && settings.enabled &&
      any_pre_dlss) {
    if (!hooks_available) {
      static bool logged_pre_hook_failure = false;
      LogOnce(logged_pre_hook_failure, reshade::log::level::warning,
              "Pre-DLSS skipped: D3D12 command-list state hooks are unavailable");
    } else {
      ID3D12Resource* color =
          GetResourceParameter(parameters, NVSDK_NGX_Parameter_Color);
      if (!IsSupportedPreDlssColor(color, parameters)) {
        static bool logged_pre_color_unsupported = false;
        LogOnce(logged_pre_color_unsupported, reshade::log::level::warning,
                "Pre-DLSS skipped: NGX Color must be a full-resource, single-sample 2D texture");
      } else {
        AddonController::Get().OnPreDlssEvaluate(
            work_list, color, exposure_texture);
        // Restore the application's command-list bindings before NGX records
        // its own work. The final restore below retains the established
        // post-evaluation behavior when another addon stage also ran.
        D3D12Hooks::Get().RestoreCommandList(work_list);
        recorded_pass = true;
      }
    }
  }

  const NVSDK_NGX_Result result = real_NVSDK_NGX_D3D12_EvaluateFeature(
      command_list, feature_handle, parameters, callback);

  static bool logged_first = false;
  if (!logged_first) {
    logged_first = true;
    char message[128];
    std::snprintf(message, sizeof(message),
                  "NGX evaluate hook fired (result=%d)",
                  static_cast<int>(result));
    reshade::log::message(reshade::log::level::info, message);
  }

  if (result == NVSDK_NGX_Result_Success && feature_handle != nullptr &&
      parameters != nullptr && feature_id == NVSDK_NGX_Feature_SuperSampling) {
    if (!hooks_available) hooks_available = EnsureHooks(work_list);
    if (!hooks_available) {
      if (any_post_dlss) {
        AddonController::Get().SetPostDlssBlocked(
            "Post-DLSS disabled: D3D12 command list hooks are not installed");
      }
    } else {
      // Rare hook-install fallback: if the pre-evaluation capture could not
      // run, refresh current guides now. A Pre-DLSS stage was already skipped
      // in this case, but Post-DLSS/Backbuffer can still use this snapshot.
      if (settings.enabled && any_pass_enabled &&
          !guides_captured_before_evaluate) {
        DlssGuides::Get().CaptureGuides(work_list, parameters, feature_id);
      }

      if (settings.enabled) {
        // Capture the current upscaled output only when PostDLSS passes need it.
        // HDR measurement runs before the first Pre/Post-DLSS pass, using the
        // published previous-frame Importance Mask. The new mask is generated
        // after the first successful model evaluation. Backbuffer-only setups
        // never enter that path.
        ID3D12Resource* output = nullptr;
        if (any_post_dlss) {
          output = GetResourceParameter(parameters, NVSDK_NGX_Parameter_Output);
          if (output != nullptr) {
            DlssGuides::Get().CaptureOutput(work_list, parameters, feature_id);
          }
        }

        if (any_post_dlss) {
          AddonController::Get().SetPostDlssBlocked(nullptr);
          if (output == nullptr) {
            output = GetResourceParameter(parameters, NVSDK_NGX_Parameter_Output);
          }
          if (output != nullptr) {
            AddonController::Get().OnPostDlssEvaluate(
                work_list, output, exposure_texture);
            recorded_pass = true;
          }
        } else {
          AddonController::Get().SetPostDlssBlocked(nullptr);
        }
      }
    }
  }

  // The restored bindings are only replayed if one of our game-list targets
  // ran. Otherwise the tracker may hold pointers from a previous recording.
  if (recorded_pass) {
    D3D12Hooks::Get().RestoreCommandList(work_list);
  }
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

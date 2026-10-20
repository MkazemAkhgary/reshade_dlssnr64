#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "addon_controller.h"
#include "profiling/profile.h"
#include "crash_trace.h"
#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "nvngx_hook_compat.h"
#include "profiling/tracy_integration.h"
#include "state/settings.h"
#include "ui.h"

namespace {

// Shared private-data contract published by ReShade's Generic Depth addon.
// The UUID and field order intentionally mirror generic_depth_data.
struct __declspec(uuid("7c6363c7-f94e-437a-9160-141782c44a98")) GenericDepthData {
  reshade::api::resource selected_depth_stencil = {};
  reshade::api::resource override_depth_stencil = {};
  reshade::api::resource_view selected_shader_resource = {};
  bool using_backup_texture = false;
};

// Roughly two seconds at 60 fps: long enough to cover a device that is not
// ready at init_device, short enough that a permanent failure stops costing
// an allocator and a command list per frame.
constexpr int kMaxDeviceHookRetries = 120;

bool g_initialized = false;
bool g_ngx_hooked = false;
bool g_ngx_hook_warned = false;
int g_ngx_hook_presents = 0;
int g_device_hook_retries = 0;


void PinAddonModule() {
  static bool pinned = false;
  if (pinned) return;

  HMODULE module = nullptr;
  const void* address_in_module = reinterpret_cast<const void*>(&PinAddonModule);
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          static_cast<LPCWSTR>(address_in_module),
          &module) != 0) {
    reshade::log::message(reshade::log::level::info, "Pinned reshade_dlssnr addon module");
    pinned = true;
  }
}

void OnInitDevice(reshade::api::device* device) {
  if (device == nullptr) return;
  auto* native_device = reinterpret_cast<ID3D12Device*>(device->get_native());
  reshade_dlssnr::D3D12Hooks::Get().InstallEarlyHooks(native_device);
}

void OnReshadeBeginEffects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list*,
    reshade::api::resource_view,
    reshade::api::resource_view) {
  if (runtime == nullptr) return;
  reshade::api::device* const device = runtime->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) {
    reshade_dlssnr::DlssGuides::Get().SetFallbackDepth(nullptr);
    return;
  }

  const GenericDepthData* const depth_data =
      runtime->get_private_data<GenericDepthData>();
  if (depth_data == nullptr || depth_data->selected_shader_resource.handle == 0) {
    reshade_dlssnr::DlssGuides::Get().SetFallbackDepth(nullptr);
    return;
  }

  const reshade::api::resource depth =
      device->get_resource_from_view(depth_data->selected_shader_resource);
  if (depth.handle == 0) {
    reshade_dlssnr::DlssGuides::Get().SetFallbackDepth(nullptr);
    return;
  }

  // ReShade resource handles map to native resource handles in D3D12.
  auto* const native_depth = reinterpret_cast<ID3D12Resource*>(
      static_cast<uintptr_t>(depth.handle));
  reshade_dlssnr::DlssGuides::Get().SetFallbackDepth(native_depth);
}

void OnPresent(
    reshade::api::command_queue* command_queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
  DLSSNR_PROFILE_FRAME_L1("DLSSNR ReShade Present");
  DLSSNR_PROFILE_CORE_L1("ReShade Present Callback");
  if (command_queue == nullptr || swapchain == nullptr) return;

  reshade::api::device* const device = command_queue->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) {
    return;
  }

  // Two installs that can miss the first time: nvngx_dlss.dll is not loaded
  // when the addon starts, and the device hook runs at init_device. Retry
  // both here, once per present, until each reports success.
  if (!g_ngx_hooked) {
    g_ngx_hooked = reshade_dlssnr::ngx_hook_compat::TryHookNgx();

    // The Pre-DLSS and Post-DLSS stages require this hook, and it used to fail
    // silently. Say so once it is clear the module is not coming.
    if (!g_ngx_hooked && ++g_ngx_hook_presents > kMaxDeviceHookRetries &&
        !g_ngx_hook_warned) {
      g_ngx_hook_warned = true;
      reshade::log::message(
          reshade::log::level::warning,
          "nvngx_dlss.dll never appeared; Pre-DLSS/Post-DLSS stages cannot run, but Backbuffer passes remain available");
    }
  }

  // Installing creates a throwaway allocator and command list, so a retry
  // that never succeeds would allocate one pair per frame forever. Cap it.
  if (!reshade_dlssnr::D3D12Hooks::Get().Installed() &&
      g_device_hook_retries < kMaxDeviceHookRetries) {
    ++g_device_hook_retries;
    reshade_dlssnr::D3D12Hooks::Get().InstallEarlyHooks(
        reinterpret_cast<ID3D12Device*>(device->get_native()));
  }

  auto* native_device = reinterpret_cast<ID3D12Device*>(device->get_native());
  auto* native_command_queue = reinterpret_cast<ID3D12CommandQueue*>(command_queue->get_native());
  auto* native_swapchain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());
  reshade_dlssnr::profiling::InitializeGpu(native_device, native_command_queue);

  reshade_dlssnr::AddonController::Get().OnPresent(
      native_command_queue,
      native_swapchain);
  reshade_dlssnr::profiling::MarkPresent(native_swapchain);
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  // First thing installed, so a fault during any of the rest is reported.
  reshade_dlssnr::crash_trace::Install();

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(
      OnReshadeBeginEffects);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_overlay("ReShade DLSSNR", reshade_dlssnr::DrawOverlay);

  // nvngx_dlss.dll is loaded on demand, either by the game directly or by
  // Streamline from the game's own directory. It is usually not there yet at
  // addon init, so the hook is retried from OnPresent until it lands.
  g_ngx_hooked = reshade_dlssnr::ngx_hook_compat::TryHookNgx();
  
  if (!g_ngx_hooked)
  {
      reshade::log::message(reshade::log::level::warning,
          "DLSSNR: NGX hook not installed yet; will retry on present");
  }

  g_initialized = true;
}

void Detach() {
  if (!g_initialized) return;

  reshade_dlssnr::ngx_hook_compat::UnhookNgx();
  g_ngx_hooked = false;

  reshade_dlssnr::AddonController::Get().Shutdown();
  reshade_dlssnr::DlssGuides::Get().Clear();
  reshade_dlssnr::SaveSettings();
  g_initialized = false;
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "ReShade DLSSNR";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Runs NVIDIA DLSS Neural Rendering over DLSS upscaling";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  if (!reshade::register_addon(addon_module, reshade_module)) {
    return false;
  }

  Attach(addon_module);
  return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
  Detach();
  reshade::unregister_addon(addon_module, reshade_module);
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
  return TRUE;
}

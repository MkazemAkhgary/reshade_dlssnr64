#include <windows.h>

#include <d3d12sdklayers.h>
#include <imgui.h>
#include <reshade.hpp>

#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "addon_controller.h"
#include "crash_trace.h"
#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "nvngx_hook_compat.h"
#include "settings.h"
#include "ui.h"

namespace {

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

void OnPresent(
    reshade::api::command_queue* command_queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*) {
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

    // The DLSS Output stage is dead without this hook, and it used to fail
    // silently. Say so once it is clear the module is not coming.
    if (!g_ngx_hooked && ++g_ngx_hook_presents > kMaxDeviceHookRetries &&
        !g_ngx_hook_warned) {
      g_ngx_hook_warned = true;
      reshade::log::message(
          reshade::log::level::warning,
          "nvngx_dlss.dll never appeared; the DLSS Output stage cannot run");
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

  auto* native_command_queue = reinterpret_cast<ID3D12CommandQueue*>(command_queue->get_native());
  auto* native_swapchain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());

  reshade_dlssnr::AddonController::Get().OnPresent(
      native_command_queue,
      native_swapchain);
}

// Opt-in D3D12 debug layer, for when a failure has to be named rather than
// guessed at. The layer has to be enabled *before* D3D12CreateDevice, and
// ReShade does not enable it, so an injected DLL is the only place it can
// happen. Off by default: it makes games crawl and floods the debug output.
void EnableDebugLayerIfRequested() {
  char* value = nullptr;
  size_t length = 0;
  if (_dupenv_s(&value, &length, "DLSSNR_D3D12_DEBUG") != 0 || value == nullptr) {
    return;
  }
  const bool requested = length > 1 && value[0] == '1';
  std::free(value);
  if (!requested) return;

  Microsoft::WRL::ComPtr<ID3D12Debug> debug;
  if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))) || debug == nullptr) {
    reshade::log::message(
        reshade::log::level::warning,
        "D3D12 debug layer requested but unavailable; install the Graphics Tools feature");
    return;
  }

  debug->EnableDebugLayer();
  reshade::log::message(reshade::log::level::info,
                        "D3D12 debug layer enabled; expect validation output and a large slowdown");
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  EnableDebugLayerIfRequested();
  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  // First thing installed, so a fault during any of the rest is reported.
  reshade_dlssnr::crash_trace::Install();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_overlay("ReShade DLSSNR", reshade_dlssnr::DrawOverlay);

  // nvngx_dlss.dll is loaded on demand, either by the game directly or by
  // Streamline from the game's own directory. It is usually not there yet at
  // addon init, so the hook is retried from OnPresent until it lands.
  g_ngx_hooked = reshade_dlssnr::ngx_hook_compat::TryHookNgx();

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

#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "dlssnr_direct.h"
#include "nvngx_hook_compat.h"
#include "settings.h"
#include "ui.h"

namespace {

bool g_initialized = false;
HMODULE g_nvngx_dlss_module = nullptr;


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

  auto* native_command_queue = reinterpret_cast<ID3D12CommandQueue*>(command_queue->get_native());
  auto* native_swapchain = reinterpret_cast<IDXGISwapChain*>(swapchain->get_native());

  reshade_dlssnr::DlssNrDirect::Get().OnPresent(
      native_command_queue,
      native_swapchain);
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
  reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
  reshade::register_event<reshade::addon_event::draw>(OnDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_overlay("ReShade DLSSNR", reshade_dlssnr::DrawOverlay);

  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) {
    nvngx_dlss = LoadLibraryW(L"nvngx_dlss.dll");
  }

  if (nvngx_dlss != nullptr) {
    if (reshade_dlssnr::local_vtable::Hook(
            nvngx_dlss,
            reshade_dlssnr::ngx_hook_compat::DLSS_HOOKS)) {
      g_nvngx_dlss_module = nvngx_dlss;
    }
  } else {
    reshade::log::message(
        reshade::log::level::warning,
        "nvngx_dlss.dll not loaded; hook skipped");
  }

  g_initialized = true;
}

void Detach() {
  if (!g_initialized) return;

  if (g_nvngx_dlss_module != nullptr) {
    reshade_dlssnr::local_vtable::Unhook(
        g_nvngx_dlss_module,
        reshade_dlssnr::ngx_hook_compat::DLSS_HOOKS);
    g_nvngx_dlss_module = nullptr;
  }

  reshade_dlssnr::DlssNrDirect::Get().Shutdown();
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

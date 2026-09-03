#include <windows.h>

#include <reshade.hpp>

#include "d3d12_hooks.h"
#include "dlssnr_pass.h"
#include "nvngx_hook_compat.h"
#include "settings.h"

namespace {

bool g_initialized = false;

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

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);

  HMODULE nvngx_dlss = GetModuleHandleW(L"nvngx_dlss.dll");
  if (nvngx_dlss == nullptr) {
    nvngx_dlss = LoadLibraryW(L"nvngx_dlss.dll");
  }

  if (nvngx_dlss != nullptr) {
    renodx::utils::vtable::Hook(
        nvngx_dlss,
        reshade_dlssnr::renodx_compat::DLSS_HOOKS);
  } else {
    reshade::log::message(
        reshade::log::level::warning,
        "nvngx_dlss.dll not loaded; hook skipped");
  }

  g_initialized = true;
}

void Detach() {
  if (!g_initialized) return;

  reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);

  reshade_dlssnr::DlssNrPass::Get().Shutdown();
  reshade_dlssnr::SaveSettings();
  g_initialized = false;
}

}  // namespace

extern "C" __declspec(dllexport) const char* NAME = "ReShade DLSSNR";
extern "C" __declspec(dllexport) const char* DESCRIPTION = "Runs NVIDIA DLSS Neural Rendering over DLSS upscaling";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE) {
  Attach(addon_module);
  return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE, HMODULE) {
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      Attach(module);
      break;
    case DLL_PROCESS_DETACH:
      if (reserved != nullptr) {
        Detach();
        reshade::unregister_addon(module);
      }
      break;
  }

  return TRUE;
}

#include <windows.h>

#include <reshade.hpp>

#include <renodx/src/utils/dlss_hook.hpp>

#include "dlssnr_pass.h"
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

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  renodx::utils::dlss_hook::nvngx_dlss_file_path = "nvngx_dlss.dll";
  renodx::utils::dlss_hook::Use(DLL_PROCESS_ATTACH);

  g_initialized = true;
}

void Detach() {
  if (!g_initialized) return;

  reshade_dlssnr::DlssNrPass::Get().Shutdown();
  reshade_dlssnr::SaveSettings();
  renodx::utils::dlss_hook::Use(DLL_PROCESS_DETACH);
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

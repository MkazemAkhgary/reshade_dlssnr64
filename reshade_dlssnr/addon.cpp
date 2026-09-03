#include <windows.h>

#include <reshade.hpp>

#include "dlssnr_pass.h"
#include "nvngx_hook.h"
#include "settings.h"

extern IMAGE_DOS_HEADER __ImageBase;

namespace {

bool g_initialized = false;
HMODULE g_nvngx_module = nullptr;

void PinAddonModule() {
  static bool pinned = false;
  if (pinned) return;

  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
          reinterpret_cast<LPCWSTR>(&__ImageBase),
          &module) != 0) {
    reshade::log::message(reshade::log::level::info, "Pinned reshade_dlssnr addon module");
    pinned = true;
  }
}

HMODULE FindNvngxDlss() {
  HMODULE module = GetModuleHandleW(L"nvngx_dlss.dll");
  if (module != nullptr) return module;
  return LoadLibraryW(L"nvngx_dlss.dll");
}

bool HookNvngxDlss(HMODULE module) {
  if (module == nullptr) return false;

  void* evaluate = GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature");
  if (evaluate == nullptr) {
    reshade::log::message(reshade::log::level::error, "nvngx_dlss.dll missing NVSDK_NGX_D3D12_EvaluateFeature");
    return false;
  }

  reshade_dlssnr::NvngxHook::real_NVSDK_NGX_D3D12_EvaluateFeature =
      reinterpret_cast<decltype(&NVSDK_NGX_D3D12_EvaluateFeature)>(evaluate);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (DetourAttach(
          reinterpret_cast<void**>(&reshade_dlssnr::NvngxHook::real_NVSDK_NGX_D3D12_EvaluateFeature),
          reshade_dlssnr::NvngxHook::hooked_NVSDK_NGX_D3D12_EvaluateFeature) != NO_ERROR) {
    DetourTransactionAbort();
    return false;
  }

  if (DetourTransactionCommit() != NO_ERROR) {
    DetourTransactionAbort();
    return false;
  }

  reshade::log::message(reshade::log::level::info, "Hooked nvngx_dlss.dll NVSDK_NGX_D3D12_EvaluateFeature");
  return true;
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  g_nvngx_module = FindNvngxDlss();
  if (!HookNvngxDlss(g_nvngx_module)) {
    reshade::log::message(reshade::log::level::error, "Failed to hook nvngx_dlss.dll");
  }

  g_initialized = true;
}

void Detach() {
  if (!g_initialized) return;

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

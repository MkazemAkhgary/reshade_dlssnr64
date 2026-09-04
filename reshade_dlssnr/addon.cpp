#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include "d3d12_hooks.h"
#include "dlss_guides.h"
#include "dlssnr_direct.h"
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

void DrawSettings(reshade::api::effect_runtime*) {
  auto& settings = reshade_dlssnr::GetSettings();
  bool changed = false;

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Only run on SuperSampling", &settings.only_super_sampling);

  ImGui::Separator();

  changed |= ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::SliderFloat("Transfer Strength", &settings.transfer_strength, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::SliderFloat("Colour Strength", &settings.colour_strength, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Working Scale", &settings.working_scale, 0.25f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Max Ratio", &settings.max_ratio, 1.0f, 8.0f, "%.2f");
  changed |= ImGui::SliderFloat("White Point Scale", &settings.white_point_scale, 0.25f, 4.0f, "%.2f");

  ImGui::Separator();

  {
    static const char* preset_names[] = {"Default", "Model A", "Model B", "Model C"};
    changed |= ImGui::Combo("Preset", &settings.preset, preset_names, IM_ARRAYSIZE(preset_names));
  }

  {
    static const char* style_names[] = {"Default (standard)", "Natural", "Cinematic"};
    changed |= ImGui::Combo("Style", &settings.style, style_names, IM_ARRAYSIZE(style_names));
  }

  changed |= ImGui::SliderFloat("Local Structure", &settings.local_structure, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::SliderFloat("Local Tone", &settings.local_tone, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::SliderFloat("Skin Structure", &settings.skin_structure, -1.0f, 2.0f, "%.2f");
  changed |= ImGui::Checkbox("Auto Mask", &settings.auto_mask);

  ImGui::Separator();

  {
    static const char* debug_names[] = {
        "Off",
        "Color (model input)",
        "Depth (guide)",
        "Motion (guide)",
        "UI Mask",
        "DLSS Stage (pre-UI)",
    };
    changed |= ImGui::Combo("Debug View", &settings.debug_view, debug_names, IM_ARRAYSIZE(debug_names));
  }

  if (changed) {
    reshade_dlssnr::SaveSettings();
  }
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::present>(OnPresent);
  reshade::register_overlay("ReShade DLSSNR", DrawSettings);

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

  reshade::unregister_overlay("ReShade DLSSNR", DrawSettings);
  reshade::unregister_event<reshade::addon_event::present>(OnPresent);
  reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);

  reshade_dlssnr::DlssNrDirect::Get().Shutdown();
  reshade_dlssnr::DlssGuides::Get().Clear();
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

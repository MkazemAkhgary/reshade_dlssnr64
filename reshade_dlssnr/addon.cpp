#include <windows.h>

#include <imgui.h>
#include <reshade.hpp>

#include "d3d12_hooks.h"
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

void OnFinishEffects(
    reshade::api::effect_runtime*,
    reshade::api::command_list* command_list,
    reshade::api::resource_view rtv,
    reshade::api::resource_view) {
  if (command_list == nullptr || rtv == 0) return;

  reshade::api::device* const device = command_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) {
    return;
  }

  auto* native_command_list = reinterpret_cast<ID3D12GraphicsCommandList*>(command_list->get_native());

  const auto target_resource = device->get_resource_from_view(rtv);
  auto* native_target = reinterpret_cast<ID3D12Resource*>(target_resource.handle);

  reshade_dlssnr::DlssNrDirect::Get().OnFinishEffects(
      native_command_list,
      native_target);
}

void DrawSettings(reshade::api::effect_runtime*) {
  auto& settings = reshade_dlssnr::GetSettings();
  bool changed = false;

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Only run on SuperSampling", &settings.only_super_sampling);

  ImGui::Separator();

  changed |= ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Transfer Strength", &settings.transfer_strength, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Colour Strength", &settings.colour_strength, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Working Scale", &settings.working_scale, 0.25f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Max Ratio", &settings.max_ratio, 1.0f, 8.0f, "%.2f");
  changed |= ImGui::SliderFloat("White Point Scale", &settings.white_point_scale, 1.0f, 1000.0f, "%.0f");

  ImGui::Separator();

  changed |= ImGui::SliderInt("Preset", &settings.preset, 0, 6);
  changed |= ImGui::SliderInt("Style", &settings.style, 0, 3);

  changed |= ImGui::SliderFloat("Local Structure", &settings.local_structure, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Local Tone", &settings.local_tone, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Skin Structure", &settings.skin_structure, -1.0f, 1.0f, "%.2f");
  changed |= ImGui::Checkbox("Auto Mask", &settings.auto_mask);

  ImGui::Separator();

  changed |= ImGui::SliderInt("Debug View", &settings.debug_view, 0, 3);

  if (changed) {
    reshade_dlssnr::SaveSettings();
  }
}

void Attach(HMODULE module) {
  if (g_initialized) return;

  reshade_dlssnr::LoadSettings();
  PinAddonModule();

  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
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
  reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::unregister_event<reshade::addon_event::init_device>(OnInitDevice);

  reshade_dlssnr::DlssNrDirect::Get().Shutdown();
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

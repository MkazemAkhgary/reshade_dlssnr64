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

namespace {

bool g_initialized = false;
bool g_freeze_render_pass_list = false;

struct RenderPassInfo {
  uint32_t index = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  uint32_t draw_count = 0;
  float viewport_width = 0.0f;
  float viewport_height = 0.0f;
  uint64_t target_handle = 0;
  bool is_backbuffer_size = false;
  bool is_backbuffer_resource = false;
};

std::mutex g_pass_mutex;
std::vector<RenderPassInfo> g_render_passes;
std::vector<RenderPassInfo> g_display_passes;
std::unordered_map<reshade::api::command_list*, size_t> g_current_pass_for_command_list;
uint32_t g_current_pass_index = 0;

reshade::api::resource_desc g_backbuffer_desc = {};
bool g_has_backbuffer_desc = false;
uint64_t g_backbuffer_native_handle = 0;
bool g_has_backbuffer_native_handle = false;

uint64_t g_selected_pass_target_handle = 0;
bool g_has_selected_pass_target = false;


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

bool OnBeginRenderPass(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::render_pass_render_target_desc* render_targets,
    const reshade::api::render_pass_depth_stencil_desc*,
    reshade::api::render_pass_flags) {
  if (command_list == nullptr || count == 0 || render_targets == nullptr) return false;

  reshade::api::device* const device = command_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return false;

  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0].view);

  if (target_resource == 0) return false;

  const reshade::api::resource_desc desc = device->get_resource_desc(target_resource);

  RenderPassInfo info;
  info.index = g_current_pass_index++;
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.target_handle = target_resource.handle;

  if (g_has_backbuffer_desc) {
    info.is_backbuffer_size =
        info.width == g_backbuffer_desc.texture.width &&
        info.height == g_backbuffer_desc.texture.height;
  }

  info.is_backbuffer_resource =
      g_has_backbuffer_native_handle &&
      info.target_handle == g_backbuffer_native_handle;

  {
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    const size_t pass_index = g_render_passes.size();
    g_render_passes.push_back(info);
    g_current_pass_for_command_list[command_list] = pass_index;
  }

  // Selected-pass capture for render-pass style games is intentionally not
  // handled here. This title uses classic bind_render_targets instead.
  return false;
}

void OnBindRenderTargets(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::resource_view* render_targets,
    reshade::api::resource_view) {
  if (command_list == nullptr || count == 0 || render_targets == nullptr) return;

  reshade::api::device* const device = command_list->get_device();
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12) return;

  const reshade::api::resource target_resource =
      device->get_resource_from_view(render_targets[0]);

  if (target_resource == 0) return;

  const reshade::api::resource_desc desc = device->get_resource_desc(target_resource);

  RenderPassInfo info;
  info.index = g_current_pass_index++;
  info.width = desc.texture.width;
  info.height = desc.texture.height;
  info.format = static_cast<uint32_t>(desc.texture.format);
  info.target_handle = target_resource.handle;

  if (g_has_backbuffer_desc) {
    info.is_backbuffer_size =
        info.width == g_backbuffer_desc.texture.width &&
        info.height == g_backbuffer_desc.texture.height;
  }

  info.is_backbuffer_resource =
      g_has_backbuffer_native_handle &&
      info.target_handle == g_backbuffer_native_handle;

  {
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    const size_t pass_index = g_render_passes.size();
    g_render_passes.push_back(info);
    g_current_pass_for_command_list[command_list] = pass_index;
  }

  if (g_has_selected_pass_target &&
      info.target_handle == g_selected_pass_target_handle) {
    auto* native_target = reinterpret_cast<ID3D12Resource*>(target_resource.handle);

    if (native_target != nullptr) {
      native_target->AddRef();
      reshade_dlssnr::DlssNrDirect::Get().SetPendingSelectedPass(native_target);
    }
  }
}

void OnBindViewports(
    reshade::api::command_list* command_list,
    uint32_t first,
    uint32_t count,
    const reshade::api::viewport* viewports) {
  if (command_list == nullptr || count == 0 || viewports == nullptr) return;

  std::lock_guard<std::mutex> lock(g_pass_mutex);

  auto it = g_current_pass_for_command_list.find(command_list);
  if (it == g_current_pass_for_command_list.end()) return;

  if (it->second >= g_render_passes.size()) return;

  RenderPassInfo& pass = g_render_passes[it->second];
  pass.viewport_width = viewports[first].width;
  pass.viewport_height = viewports[first].height;
}

bool OnDraw(
    reshade::api::command_list* command_list,
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t) {
  if (command_list == nullptr) return false;

  std::lock_guard<std::mutex> lock(g_pass_mutex);

  auto it = g_current_pass_for_command_list.find(command_list);
  if (it == g_current_pass_for_command_list.end()) return false;
  if (it->second >= g_render_passes.size()) return false;

  g_render_passes[it->second].draw_count++;
  return false;
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    uint32_t,
    uint32_t,
    uint32_t,
    int32_t,
    uint32_t) {
  if (command_list == nullptr) return false;

  std::lock_guard<std::mutex> lock(g_pass_mutex);

  auto it = g_current_pass_for_command_list.find(command_list);
  if (it == g_current_pass_for_command_list.end()) return false;
  if (it->second >= g_render_passes.size()) return false;

  g_render_passes[it->second].draw_count++;
  return false;
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

  // Capture backbuffer description and reset render-pass tracking for next frame.
  reshade::api::device* const present_device = command_queue->get_device();
  if (present_device != nullptr && present_device->get_api() == reshade::api::device_api::d3d12) {
    const auto backbuffer_handle = swapchain->get_current_back_buffer();
    if (backbuffer_handle != 0) {
      g_backbuffer_desc = present_device->get_resource_desc(backbuffer_handle);
      g_has_backbuffer_desc = true;
      g_backbuffer_native_handle = backbuffer_handle.handle;
      g_has_backbuffer_native_handle = true;
    }
  }

  if (!g_freeze_render_pass_list) {
    std::lock_guard<std::mutex> lock(g_pass_mutex);
    g_display_passes.swap(g_render_passes);
    g_render_passes.clear();
    g_current_pass_for_command_list.clear();
    g_current_pass_index = 0;
  }

}

void DrawSettings(reshade::api::effect_runtime*) {
  auto& settings = reshade_dlssnr::GetSettings();
  bool changed = false;

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Only run on SuperSampling", &settings.only_super_sampling);

  ImGui::Separator();

  {
    static const char* model_names[] = {"Model A", "Model B", "Model C"};
    changed |= ImGui::Combo("Model", &settings.preset, model_names, IM_ARRAYSIZE(model_names));
  }

  changed |= ImGui::SliderFloat("Local Tone", &settings.local_tone, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::Checkbox("UI Correction", &settings.ui_correction);

  ImGui::Separator();

  changed |= ImGui::SliderFloat("Local Structure", &settings.local_structure, 0.0f, 2.0f, "%.2f");
  const bool character_mask_changed =
      ImGui::Checkbox("Character Mask", &settings.auto_mask);

  changed |= character_mask_changed;
  if (character_mask_changed && !settings.auto_mask) {
    settings.skin_structure = -1.0f;
  }

  if (!settings.auto_mask) {
    ImGui::BeginDisabled();
  }
  changed |= ImGui::SliderFloat("Skin Structure", &settings.skin_structure, 0.0f, 1.0f, "%.2f");
  if (!settings.auto_mask) {
    ImGui::EndDisabled();
  }

  ImGui::Separator();

  changed |= ImGui::SliderFloat("Intensity", &settings.intensity, 0.0f, 2.0f, "%.2f");
  changed |= ImGui::SliderFloat("Luma Strength", &settings.luma_strength, 0.0f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Color Strength", &settings.color_strength, 0.0f, 1.0f, "%.2f");

  ImGui::Separator();

  changed |= ImGui::SliderFloat("Working Scale", &settings.working_scale, 0.25f, 1.0f, "%.2f");
  changed |= ImGui::SliderFloat("Max Ratio", &settings.max_ratio, 1.0f, 8.0f, "%.2f");
  changed |= ImGui::SliderFloat("White Point Scale", &settings.white_point_scale, 0.25f, 4.0f, "%.2f");

  ImGui::Separator();

  changed |= ImGui::Checkbox("Use selected pass as model input", &settings.use_selected_pass_as_input);
  changed |= ImGui::SliderFloat("UI Mask Threshold", &settings.ui_mask_threshold, 0.0f, 0.2f, "%.3f");

  {
    static const char* debug_names[] = {
        "Off",
        "Color (model input)",
        "Depth (guide)",
        "Motion (guide)",
        "UI Mask",
        "DLSS Stage (pre-UI)",
        "Selected Pass Resource",
    };
    changed |= ImGui::Combo("Debug View", &settings.debug_view, debug_names, IM_ARRAYSIZE(debug_names));

    if (settings.debug_view == 4) {
      if (settings.use_selected_pass_as_input) {
        ImGui::TextUnformatted("UI Mask: composited manually after model.");
      } else {
        ImGui::TextUnformatted("UI Mask: passed to model.");
      }
    }
  }

  ImGui::Separator();

  static bool first_pass_frame = true;
  if (first_pass_frame) {
    ImGui::SetNextItemOpen(false, ImGuiCond_Once);
    first_pass_frame = false;
  }

  if (ImGui::CollapsingHeader("Render Passes")) {
    ImGui::Checkbox("Freeze render pass list", &g_freeze_render_pass_list);

  std::lock_guard<std::mutex> pass_lock(g_pass_mutex);

  if (g_has_backbuffer_desc) {
    ImGui::Text(
        "Backbuffer: %ux%u format=%u native=0x%llx",
        g_backbuffer_desc.texture.width,
        g_backbuffer_desc.texture.height,
        static_cast<uint32_t>(g_backbuffer_desc.texture.format),
        static_cast<unsigned long long>(g_backbuffer_native_handle));
  }

  static bool filter_backbuffer_resource = false;
  static bool filter_full_viewport = false;
  static int max_draw_count = 999;

  ImGui::Checkbox("Only actual backbuffer resource", &filter_backbuffer_resource);
  ImGui::Checkbox("Only full viewport", &filter_full_viewport);
  ImGui::SliderInt("Max draw count", &max_draw_count, 0, 500);

  ImGui::Separator();

  for (size_t i = 0; i < g_display_passes.size(); ++i) {
    const RenderPassInfo& pass = g_display_passes[i];

    if (filter_backbuffer_resource && !pass.is_backbuffer_resource) continue;

    if (filter_full_viewport &&
        !(pass.viewport_width >= static_cast<float>(pass.width) - 1.0f &&
          pass.viewport_height >= static_cast<float>(pass.height) - 1.0f)) {
      continue;
    }

    if (pass.draw_count > static_cast<uint32_t>(max_draw_count)) continue;

    std::string label;
    label += std::to_string(static_cast<unsigned long long>(pass.index));
    label += ": ";
    label += std::to_string(pass.width);
    label += "x";
    label += std::to_string(pass.height);
    label += " fmt=";
    label += std::to_string(pass.format);
    label += " draws=";
    label += std::to_string(pass.draw_count);
    label += " vp=";
    label += std::to_string(static_cast<int>(pass.viewport_width));
    label += "x";
    label += std::to_string(static_cast<int>(pass.viewport_height));
    label += " id=0x";

    char id_buffer[32] = {};
    sprintf_s(id_buffer, "%llx", static_cast<unsigned long long>(pass.target_handle));
    label += id_buffer;

    if (pass.is_backbuffer_resource) {
      label += " [BB]";
    }

    if (g_has_selected_pass_target && pass.target_handle == g_selected_pass_target_handle) {
      label += " <selected>";
    }

    const bool is_selected =
        g_has_selected_pass_target &&
        pass.target_handle == g_selected_pass_target_handle;

    if (ImGui::Selectable(label.c_str(), is_selected)) {
      if (is_selected) {
        g_selected_pass_target_handle = 0;
        g_has_selected_pass_target = false;
        reshade_dlssnr::DlssNrDirect::Get().SetDebugSelectedResource(0);
        reshade_dlssnr::DlssGuides::Get().SetSelectedPassHandle(0);
      } else {
        g_selected_pass_target_handle = pass.target_handle;
        g_has_selected_pass_target = true;
        reshade_dlssnr::DlssNrDirect::Get().SetDebugSelectedResource(g_selected_pass_target_handle);
        reshade_dlssnr::DlssGuides::Get().SetSelectedPassHandle(g_selected_pass_target_handle);
      }
    }
  }

  if (g_has_selected_pass_target) {
    ImGui::Separator();
    ImGui::Text(
        "Selected pass target: 0x%llx",
        static_cast<unsigned long long>(g_selected_pass_target_handle));
    ImGui::SameLine();
    if (ImGui::SmallButton("Deselect")) {
      g_selected_pass_target_handle = 0;
      g_has_selected_pass_target = false;
      reshade_dlssnr::DlssNrDirect::Get().SetDebugSelectedResource(0);
      reshade_dlssnr::DlssGuides::Get().SetSelectedPassHandle(0);
    }
  }
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
  reshade::register_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
  reshade::register_event<reshade::addon_event::bind_viewports>(OnBindViewports);
  reshade::register_event<reshade::addon_event::draw>(OnDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
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
  reshade::unregister_event<reshade::addon_event::draw>(OnDraw);
  reshade::unregister_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
  reshade::unregister_event<reshade::addon_event::bind_viewports>(OnBindViewports);
  reshade::unregister_event<reshade::addon_event::begin_render_pass>(OnBeginRenderPass);
  reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
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

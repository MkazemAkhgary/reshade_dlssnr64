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
#include "render_pass_tracker.h"
#include "settings.h"

namespace {

bool g_initialized = false;

reshade_dlssnr::RenderPassTracker g_render_pass_tracker;


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
  g_render_pass_tracker.RecordBeginRenderPass(command_list, count, render_targets);
  return false;
}

void OnBindRenderTargets(
    reshade::api::command_list* command_list,
    uint32_t count,
    const reshade::api::resource_view* render_targets,
    reshade::api::resource_view) {
  g_render_pass_tracker.RecordBindRenderTargets(command_list, count, render_targets);
}

void OnBindViewports(
    reshade::api::command_list* command_list,
    uint32_t first,
    uint32_t count,
    const reshade::api::viewport* viewports) {
  g_render_pass_tracker.RecordViewports(command_list, first, count, viewports);
}

bool OnDraw(
    reshade::api::command_list* command_list,
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t) {
  g_render_pass_tracker.RecordDraw(command_list);
  return false;
}

bool OnDrawIndexed(
    reshade::api::command_list* command_list,
    uint32_t,
    uint32_t,
    uint32_t,
    int32_t,
    uint32_t) {
  g_render_pass_tracker.RecordDrawIndexed(command_list);
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

  if (reshade_dlssnr::GetSettings().second_pass_enabled) {
    reshade_dlssnr::DlssNrDirect::GetPass2().OnPresent(
        native_command_queue,
        native_swapchain);
  }

  g_render_pass_tracker.CaptureBackbuffer(swapchain);
  g_render_pass_tracker.SwapForNextFrame();
}

void DrawSettings(reshade::api::effect_runtime*) {
  auto& settings = reshade_dlssnr::GetSettings();
  bool changed = false;

  const float kControlLabelWidth = 220.0f;

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Only run on SuperSampling", &settings.only_super_sampling);

  if (settings.enabled) {
    changed |= ImGui::Checkbox("Enable Second Pass", &settings.second_pass_enabled);
  }

  ImGui::Separator();

  auto draw_pass_settings = [&](const char* tab_label,
                                reshade_dlssnr::PassSettings& pass,
                                bool show_ui_options) {
    if (ImGui::BeginTabItem(tab_label)) {
      ImGui::PushID(tab_label);

      const float button_width =
          (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 3.0f -
          ImGui::GetStyle().ItemSpacing.x;

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Model");
      ImGui::SameLine(kControlLabelWidth);

      static const char* model_short_names[] = {"A", "B", "C"};
      for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

        const bool selected = pass.preset == i;
        if (selected) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button(model_short_names[i], ImVec2(button_width, 0.0f)) &&
            pass.preset != i) {
          pass.preset = i;
          changed = true;
        }
        if (selected) {
          ImGui::PopStyleColor();
        }
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Intensity");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Intensity", &pass.intensity, 0.0f, 2.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Intensity", ImVec2(24.0f, 0.0f))) {
        pass.intensity = 0.50f;
        changed = true;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Local Tone");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Local Tone", &pass.local_tone, 0.0f, 2.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Local Tone", ImVec2(24.0f, 0.0f))) {
        pass.local_tone = 1.0f;
        changed = true;
      }

      {
        const float input_button_width =
            (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
            ImGui::GetStyle().ItemSpacing.x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("UI Correction");
        ImGui::SameLine(kControlLabelWidth);

        const bool on = pass.ui_correction;
        const bool off = !pass.ui_correction;

        if (off) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button("OFF##UI Correction", ImVec2(input_button_width, 0.0f))) {
          pass.ui_correction = false;
          changed = true;
        }
        if (off) {
          ImGui::PopStyleColor();
        }

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
        if (on) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button("ON##UI Correction", ImVec2(input_button_width, 0.0f))) {
          pass.ui_correction = true;
          changed = true;
        }
        if (on) {
          ImGui::PopStyleColor();
        }
      }

      ImGui::Separator();

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Local Structure");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Local Structure", &pass.local_structure, 0.0f, 2.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Local Structure", ImVec2(24.0f, 0.0f))) {
        pass.local_structure = 1.0f;
        changed = true;
      }

      {
        const float input_button_width =
            (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
            ImGui::GetStyle().ItemSpacing.x;

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Character Mask");
        ImGui::SameLine(kControlLabelWidth);

        const bool on = pass.auto_mask;
        const bool off = !pass.auto_mask;

        if (off) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button("OFF##Character Mask", ImVec2(input_button_width, 0.0f))) {
          pass.auto_mask = false;
          changed = true;
        }
        if (off) {
          ImGui::PopStyleColor();
        }

        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
        if (on) {
          ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::Button("ON##Character Mask", ImVec2(input_button_width, 0.0f))) {
          pass.auto_mask = true;
          changed = true;
        }
        if (on) {
          ImGui::PopStyleColor();
        }
      }

      if (!pass.auto_mask) {
        ImGui::BeginDisabled();
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Skin Structure");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Skin Structure", &pass.skin_structure, 0.0f, 1.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Skin Structure", ImVec2(24.0f, 0.0f))) {
        pass.skin_structure = 0.0f;
        changed = true;
      }
      if (!pass.auto_mask) {
        ImGui::EndDisabled();
      }

      ImGui::Separator();

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Luma Strength");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Luma Strength", &pass.luma_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Luma Strength", ImVec2(24.0f, 0.0f))) {
        pass.luma_strength = 1.0f;
        changed = true;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Chroma Strength");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Chroma Strength", &pass.chroma_strength, 0.0f, 1.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Chroma Strength", ImVec2(24.0f, 0.0f))) {
        pass.chroma_strength = 1.0f;
        changed = true;
      }

      ImGui::Separator();

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Working Scale");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Working Scale", &pass.working_scale, 0.25f, 1.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Working Scale", ImVec2(24.0f, 0.0f))) {
        pass.working_scale = 1.0f;
        changed = true;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Residual Suppression");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Residual Suppression", &pass.residual_suppression, 0.0f, 1.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##Residual Suppression", ImVec2(24.0f, 0.0f))) {
        pass.residual_suppression = 0.85f;
        changed = true;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Residual Threshold");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##Residual Threshold", &pass.residual_threshold, 0.0f, 0.2f, "%.3f");
      ImGui::SameLine();
      if (ImGui::Button("R##Residual Threshold", ImVec2(24.0f, 0.0f))) {
        pass.residual_threshold = 0.02f;
        changed = true;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("White Point Scale");
      ImGui::SameLine(kControlLabelWidth);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
      changed |= ImGui::SliderFloat("##White Point Scale", &pass.white_point_scale, 0.25f, 4.0f, "%.2f");
      ImGui::SameLine();
      if (ImGui::Button("R##White Point Scale", ImVec2(24.0f, 0.0f))) {
        pass.white_point_scale = 100.0f;
        changed = true;
      }

      ImGui::PopID();
      ImGui::EndTabItem();
    }
  };

  if (ImGui::BeginTabBar("PassTabs")) {
    draw_pass_settings("Pass 1", settings.pass1, true);
    if (settings.second_pass_enabled) {
      draw_pass_settings("Pass 2", settings.pass2, false);
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();

  {
    const float input_button_width = (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
                                     ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model Input");
    ImGui::SameLine(kControlLabelWidth);

    const bool selected_pass_active = settings.use_selected_pass_as_input;
    const bool backbuffer_active = !settings.use_selected_pass_as_input;

    if (selected_pass_active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Selected Pass", ImVec2(input_button_width, 0.0f))) {
      settings.use_selected_pass_as_input = true;
      changed = true;
    }
    if (selected_pass_active) {
      ImGui::PopStyleColor();
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

    if (backbuffer_active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Backbuffer", ImVec2(input_button_width, 0.0f))) {
      settings.use_selected_pass_as_input = false;
      changed = true;
    }
    if (backbuffer_active) {
      ImGui::PopStyleColor();
    }
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("UI Mask Threshold");
  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
  changed |= ImGui::SliderFloat("##UI Mask Threshold", &settings.ui_mask_threshold, 0.0f, 0.2f, "%.3f");
  ImGui::SameLine();
  if (ImGui::Button("R##UI Mask Threshold", ImVec2(24.0f, 0.0f))) {
    settings.ui_mask_threshold = 0.02f;
    changed = true;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("UI Mask Softness");
  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
  changed |= ImGui::SliderFloat("##UI Mask Softness", &settings.ui_mask_softness, 0.0f, 0.2f, "%.3f");
  ImGui::SameLine();
  if (ImGui::Button("R##UI Mask Softness", ImVec2(24.0f, 0.0f))) {
    settings.ui_mask_softness = 0.06f;
    changed = true;
  }

  {
    static const char* debug_names[] = {
        "Off",
        "Color (model input)",
        "Depth (guide)",
        "Motion (guide)",
        "UI Mask",
        "DLSS Stage (pre-UI)",
        "Selected Pass Resource",
        "Working Input",
        "Working Output",
        "Working Residual",
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Debug View");
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::Combo("##Debug View", &settings.debug_view, debug_names, IM_ARRAYSIZE(debug_names));

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
    bool freeze = g_render_pass_tracker.IsFreezeRenderPassList();
    if (ImGui::Checkbox("Freeze render pass list", &freeze)) {
      g_render_pass_tracker.SetFreezeRenderPassList(freeze);
    }

    ImGui::BeginChild("##RenderPassScrolling", ImVec2(0.0f, 300.0f), true);

  if (g_render_pass_tracker.HasBackbufferDescription()) {
    const auto& backbuffer_desc = g_render_pass_tracker.BackbufferDescription();
    ImGui::Text(
        "Backbuffer: %ux%u format=%u native=0x%llx",
        backbuffer_desc.texture.width,
        backbuffer_desc.texture.height,
        static_cast<uint32_t>(backbuffer_desc.texture.format),
        static_cast<unsigned long long>(
            g_render_pass_tracker.BackbufferNativeHandle()));
  }

  const auto& display_passes = g_render_pass_tracker.DisplayPasses();
  for (size_t i = 0; i < display_passes.size(); ++i) {
    const auto& pass = display_passes[i];

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

    const bool is_selected =
        g_render_pass_tracker.IsSelectedPassSet() &&
        pass.target_handle == g_render_pass_tracker.SelectedPassHandle();

    if (is_selected) {
      label += " <selected>";
    }

    if (ImGui::Selectable(label.c_str(), is_selected)) {
      if (is_selected) {
        g_render_pass_tracker.DeselectPass();
        reshade_dlssnr::DlssNrDirect::Get().ClearSelectedPass();
        reshade_dlssnr::DlssGuides::Get().ClearSelectedPass();
      } else {
        g_render_pass_tracker.SelectPass(pass.target_handle);
        reshade_dlssnr::DlssNrDirect::Get().SetDebugSelectedResource(
            pass.target_handle);
        reshade_dlssnr::DlssGuides::Get().SetSelectedPassHandle(
            pass.target_handle);
      }
    }
  }

  if (g_render_pass_tracker.IsSelectedPassSet()) {
    ImGui::Separator();
    ImGui::Text(
        "Selected pass target: 0x%llx",
        static_cast<unsigned long long>(
            g_render_pass_tracker.SelectedPassHandle()));
    ImGui::SameLine();
    if (ImGui::SmallButton("Deselect")) {
      g_render_pass_tracker.DeselectPass();
      reshade_dlssnr::DlssNrDirect::Get().ClearSelectedPass();
      reshade_dlssnr::DlssGuides::Get().ClearSelectedPass();
    }
  }

    ImGui::EndChild();
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
  reshade_dlssnr::DlssNrDirect::GetPass2().Shutdown();
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

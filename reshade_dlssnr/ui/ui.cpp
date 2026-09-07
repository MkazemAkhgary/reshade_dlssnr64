#include <imgui.h>

#include "ui.h"

#include <string>
#include <vector>

#include "dlss_guides.h"
#include "dlssnr_direct.h"
#include "render_pass_tracker.h"
#include "settings.h"
#include "version.h"

namespace reshade_dlssnr {

namespace {

constexpr float kControlLabelWidth = 220.0f;

void DrawPassSettings(
    const char* tab_label,
    PassSettings& pass,
    bool& changed) {
  if (!ImGui::BeginTabItem(tab_label)) return;

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
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
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

  auto draw_slider = [&](const char* label, float* value,
                         float min_value, float max_value,
                         float reset_value, const char* format) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    const std::string slider_id = "##" + std::string(label);
    const std::string reset_id = "R##" + std::string(label);
    changed |= ImGui::SliderFloat(slider_id.c_str(), value,
                                  min_value, max_value, format);
    ImGui::SameLine();
    if (ImGui::Button(reset_id.c_str(), ImVec2(24.0f, 0.0f))) {
      *value = reset_value;
      changed = true;
    }
  };

  draw_slider("Intensity", &pass.intensity, 0.0f, 2.0f, 1.0f, "%.2f");
  draw_slider("Local Tone", &pass.local_tone, 0.0f, 2.0f, 1.0f, "%.2f");

  {
    const float input_button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("UI Correction");
    ImGui::SameLine(kControlLabelWidth);

    ImGui::PushID("UI Correction");

    const bool on = pass.ui_correction;
    const bool off = !pass.ui_correction;

    if (off) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("OFF", ImVec2(input_button_width, 0.0f))) {
      pass.ui_correction = false;
      changed = true;
    }
    if (off) {
      ImGui::PopStyleColor();
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("ON", ImVec2(input_button_width, 0.0f))) {
      pass.ui_correction = true;
      changed = true;
    }
    if (on) {
      ImGui::PopStyleColor();
    }

    ImGui::PopID();
  }

  ImGui::Separator();

  draw_slider("Local Structure", &pass.local_structure,
              0.0f, 2.0f, 1.0f, "%.2f");

  {
    const float input_button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Character Mask");
    ImGui::SameLine(kControlLabelWidth);

    ImGui::PushID("Character Mask");

    const bool on = pass.auto_mask;
    const bool off = !pass.auto_mask;

    if (off) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("OFF", ImVec2(input_button_width, 0.0f))) {
      pass.auto_mask = false;
      changed = true;
    }
    if (off) {
      ImGui::PopStyleColor();
    }

    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("ON", ImVec2(input_button_width, 0.0f))) {
      pass.auto_mask = true;
      changed = true;
    }
    if (on) {
      ImGui::PopStyleColor();
    }

    ImGui::PopID();
  }

  if (!pass.auto_mask) {
    ImGui::BeginDisabled();
  }
  draw_slider("Skin Structure", &pass.skin_structure,
              0.0f, 1.0f, 0.0f, "%.2f");
  if (!pass.auto_mask) {
    ImGui::EndDisabled();
  }

  ImGui::Separator();

  draw_slider("Luma Strength", &pass.luma_strength,
              0.0f, 1.0f, 1.0f, "%.2f");
  draw_slider("Chroma Strength", &pass.chroma_strength,
              0.0f, 1.0f, 1.0f, "%.2f");

  ImGui::Separator();

  draw_slider("Working Scale", &pass.working_scale,
              0.25f, 1.0f, 1.0f, "%.2f");
  draw_slider("Residual Suppression", &pass.residual_suppression,
              0.0f, 1.0f, 0.85f, "%.2f");
  draw_slider("Residual Threshold", &pass.residual_threshold,
              0.0f, 0.2f, 0.02f, "%.3f");

  ImGui::PopID();
  ImGui::EndTabItem();
}

}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  bool changed = false;

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Only run on SuperSampling",
                             &settings.only_super_sampling);

  if (settings.enabled) {
    changed |= ImGui::Checkbox("Enable Second Pass",
                               &settings.second_pass_enabled);
  }

  ImGui::Separator();

  if (ImGui::BeginTabBar("PassTabs")) {
    DrawPassSettings("Pass 1", settings.pass1, changed);
    if (settings.second_pass_enabled) {
      DrawPassSettings("Pass 2", settings.pass2, changed);
    }
    ImGui::EndTabBar();
  }

  ImGui::Separator();

  {
    const float input_button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Model Input");
    ImGui::SameLine(kControlLabelWidth);

    const bool selected_pass_active = settings.use_selected_pass_as_input;
    const bool backbuffer_active = !settings.use_selected_pass_as_input;

    if (selected_pass_active) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
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
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("Backbuffer", ImVec2(input_button_width, 0.0f))) {
      settings.use_selected_pass_as_input = false;
      changed = true;
    }
    if (backbuffer_active) {
      ImGui::PopStyleColor();
    }
  }

  auto draw_slider = [&](const char* label, float* value,
                         float min_value, float max_value,
                         float reset_value, const char* format) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    const std::string slider_id = "##" + std::string(label);
    const std::string reset_id = "R##" + std::string(label);
    changed |= ImGui::SliderFloat(slider_id.c_str(), value,
                                  min_value, max_value, format);
    ImGui::SameLine();
    if (ImGui::Button(reset_id.c_str(), ImVec2(24.0f, 0.0f))) {
      *value = reset_value;
      changed = true;
    }
  };

  draw_slider("UI Mask Threshold", &settings.ui_mask_threshold,
              0.0f, 0.2f, 0.02f, "%.3f");
  draw_slider("UI Mask Softness", &settings.ui_mask_softness,
              0.0f, 0.2f, 0.06f, "%.3f");

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
    changed |= ImGui::Combo("##Debug View", &settings.debug_view,
                            debug_names, IM_ARRAYSIZE(debug_names));

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
    RenderPassTracker& tracker = RenderPassTracker::Get();

    bool freeze = tracker.IsFreezeRenderPassList();
    if (ImGui::Checkbox("Freeze render pass list", &freeze)) {
      tracker.SetFreezeRenderPassList(freeze);
    }

    ImGui::BeginChild("##RenderPassScrolling", ImVec2(0.0f, 300.0f), true);

    if (tracker.HasBackbufferDescription()) {
      const auto& desc = tracker.BackbufferDescription();
      ImGui::Text(
          "Backbuffer: %ux%u format=%u native=0x%llx",
          desc.texture.width,
          desc.texture.height,
          static_cast<uint32_t>(desc.texture.format),
          static_cast<unsigned long long>(tracker.BackbufferNativeHandle()));
    }

    const auto& display_passes = tracker.DisplayPasses();
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
      sprintf_s(id_buffer, "%llx",
                static_cast<unsigned long long>(pass.target_handle));
      label += id_buffer;

      if (pass.is_backbuffer_resource) {
        label += " [BB]";
      }

      const bool is_selected =
          tracker.IsSelectedPassSet() &&
          pass.target_handle == tracker.SelectedPassHandle();

      if (is_selected) {
        label += " <selected>";
      }

      if (ImGui::Selectable(label.c_str(), is_selected)) {
        if (is_selected) {
          tracker.DeselectPass();
          DlssNrDirect::Get().ClearSelectedPass();
          DlssGuides::Get().ClearSelectedPass();
        } else {
          tracker.SelectPass(pass.target_handle);
          DlssNrDirect::Get().SetDebugSelectedResource(pass.target_handle);
          DlssGuides::Get().SetSelectedPassHandle(pass.target_handle);
        }
      }
    }

    if (tracker.IsSelectedPassSet()) {
      ImGui::Separator();
      ImGui::Text(
          "Selected pass target: 0x%llx",
          static_cast<unsigned long long>(tracker.SelectedPassHandle()));
      ImGui::SameLine();
      if (ImGui::SmallButton("Deselect")) {
        tracker.DeselectPass();
        DlssNrDirect::Get().ClearSelectedPass();
        DlssGuides::Get().ClearSelectedPass();
      }
    }

    ImGui::EndChild();
  }

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

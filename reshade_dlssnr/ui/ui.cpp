#include <imgui.h>

#include "ui.h"

#include <string>
#include <vector>

#include "dlss_guides.h"
#include "dlssnr_direct.h"
#include "settings.h"
#include "shader_params.h"
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

  auto draw_slider = [&](const char* label, const char* tooltip,
                         float* value,
                         float min_value, float max_value,
                         float reset_value, const char* format) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    const std::string slider_id = "##" + std::string(label);
    const std::string reset_id = "R##" + std::string(label);
    changed |= ImGui::SliderFloat(slider_id.c_str(), value,
                                  min_value, max_value, format);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
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

  draw_slider("Working Scale", &pass.working_scale,
              0.25f, 1.0f, 1.0f, "%.2f");

  ImGui::Separator();

  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    const auto& param = kShaderParams[i];
    if (param.is_checkbox) {
      bool checked = pass.params[i] > 0.5f;
      changed |= ImGui::Checkbox(param.name, &checked);
      pass.params[i] = checked ? 1.0f : 0.0f;
    } else {
      draw_slider(param.name, &pass.params[i],
                  param.min_value, param.max_value,
                  param.default_value, "%.2f");
    }
  }

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

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

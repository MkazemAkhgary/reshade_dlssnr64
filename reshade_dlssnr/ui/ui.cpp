#include <imgui.h>

#include "ui.h"

#include <cstdio>
#include <cstring>
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

void DrawToggle(const char* label, const char* tooltip,
                bool* value, bool& changed) {
  const float input_button_width =
      (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 2.0f -
      ImGui::GetStyle().ItemSpacing.x;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
  ImGui::SameLine(kControlLabelWidth);

  ImGui::PushID(label);

  const bool on = *value;

  if (!on) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("OFF", ImVec2(input_button_width, 0.0f))) {
    *value = false;
    changed = true;
  }
  if (!on) {
    ImGui::PopStyleColor();
  }

  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("ON", ImVec2(input_button_width, 0.0f))) {
    *value = true;
    changed = true;
  }
  if (on) {
    ImGui::PopStyleColor();
  }

  ImGui::PopID();
}

void DrawPassSettings(
    const char* tab_label,
    PassSettings& pass,
    bool enabled,
    bool& changed) {
  if (!ImGui::BeginTabItem(tab_label)) return;

  ImGui::PushID(tab_label);

  if (!enabled) {
    ImGui::BeginDisabled();
  }

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

  draw_slider("Intensity", nullptr, &pass.intensity, 0.0f, 2.0f, 1.0f, "%.2f");
  draw_slider("Local Tone", nullptr, &pass.local_tone, 0.0f, 2.0f, 1.0f, "%.2f");

  DrawToggle("UI Correction", nullptr, &pass.ui_correction, changed);

  ImGui::Separator();

  draw_slider("Local Structure", nullptr, &pass.local_structure,
              0.0f, 2.0f, 1.0f, "%.2f");

  DrawToggle("Character Mask", nullptr, &pass.auto_mask, changed);

  if (!pass.auto_mask) {
    ImGui::BeginDisabled();
  }
  draw_slider("Skin Structure", nullptr, &pass.skin_structure,
              0.0f, 1.0f, 0.0f, "%.2f");
  if (!pass.auto_mask) {
    ImGui::EndDisabled();
  }

  ImGui::Separator();

  draw_slider("Working Scale", nullptr, &pass.working_scale,
              0.25f, 1.0f, 1.0f, "%.2f");

  ImGui::Separator();

  // Group dynamic shader params by category.
  const char* last_category = nullptr;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    const auto& param = kShaderParams[i];
    if (last_category == nullptr ||
        strcmp(last_category, param.category) != 0) {
      if (last_category != nullptr) ImGui::Separator();
      ImGui::TextUnformatted(param.category);
      last_category = param.category;
    }

    if (param.is_checkbox) {
      bool checked = pass.params[i] > 0.5f;
      changed |= ImGui::Checkbox(param.name, &checked);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", param.tooltip);
      }
      pass.params[i] = checked ? 1.0f : 0.0f;
    } else {
      draw_slider(param.name, param.tooltip, &pass.params[i],
                  param.min_value, param.max_value,
                  param.default_value, "%.2f");
    }
  }

  if (!enabled) {
    ImGui::EndDisabled();
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
    for (uint32_t i = 1; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Enable Pass %u", i + 1);
      changed |= ImGui::Checkbox(label, &settings.pass_enabled[i]);
    }
  }

  ImGui::Separator();

  if (ImGui::BeginTabBar("PassTabs")) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Pass %u", i + 1);
      DrawPassSettings(label, settings.passes[i],
                       settings.pass_enabled[i], changed);
    }
    ImGui::EndTabBar();
  }

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

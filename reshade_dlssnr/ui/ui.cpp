#include <imgui.h>

#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "compute/shader_params.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "settings.h"
#include "version.h"

namespace reshade_dlssnr {

namespace {

constexpr float kControlLabelWidth = 220.0f;

void DrawHookSelector(
    Settings& settings,
    uint32_t pass_index,
    bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  // A pass can be At DLSS only if every earlier pass is also At DLSS.
  bool can_be_at_dlss = true;
  for (uint32_t j = 0; j < pass_index; ++j) {
    if (settings.passes[j].injection_stage == InjectionStage::Present) {
      can_be_at_dlss = false;
      break;
    }
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  ImGui::SameLine(kControlLabelWidth);

  const float hook_button_width =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) /
      2.0f;

  const bool at_dlss = pass.injection_stage == InjectionStage::PostDLSS;
  if (!can_be_at_dlss) ImGui::BeginDisabled();
  if (at_dlss) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("DLSS Output", ImVec2(hook_button_width, 0.0f)) &&
      can_be_at_dlss && !at_dlss) {
    pass.injection_stage = InjectionStage::PostDLSS;
    changed = true;
  }
  if (at_dlss) ImGui::PopStyleColor();
  if (!can_be_at_dlss) ImGui::EndDisabled();

  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

  const bool at_present = pass.injection_stage == InjectionStage::Present;
  if (at_present) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("Backbuffer", ImVec2(hook_button_width, 0.0f)) &&
      !at_present) {
    pass.injection_stage = InjectionStage::Present;
    // Force all later passes to Present as well.
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
  }
  if (at_present) ImGui::PopStyleColor();
}

void DrawPassSettingsBody(
    uint32_t pass_index,
    Settings& settings,
    bool enabled,
    bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  ImGui::PushID(static_cast<int>(pass_index));

  if (!enabled) {
    ImGui::BeginDisabled();
  }

  // --- Structural: where and at what resolution the pass runs ---
  DrawHookSelector(settings, pass_index, changed);

  auto draw_slider = [&](const char* id, const char* label,
                         const char* tooltip, float* value,
                         float min_value, float max_value,
                         float reset_value) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    const std::string slider_id = std::string("##") + id;
    changed |= ImGui::SliderFloat(slider_id.c_str(), value,
                                  min_value, max_value, "%.2f");
    if (tooltip != nullptr && ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    ImGui::SameLine();
    ImGui::PushID(id);
    if (ImGui::Button("R", ImVec2(24.0f, 0.0f))) {
      *value = reset_value;
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Reset to default");
    }
    ImGui::PopID();
  };

  draw_slider("working_scale", "Working Scale",
              "Model evaluation resolution as a fraction of the frame. "
              "1.0 runs the model at full resolution.",
              &pass.working_scale, 0.25f, 1.0f, 1.0f);

  // Debug view selector. Auto-populated from the debug view registry, so
  // adding or removing a view is a single-table-edit change.
  {
    std::vector<const char*> names;
    names.reserve(8);
    names.push_back("Off");
    for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
      names.push_back(kDebugViews[i].name);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Debug View");
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::Combo("##debug_view", &pass.debug_view,
                            names.data(), static_cast<int>(names.size()));
  }

  ImGui::Separator();

  // --- Model tuning section ---
  if (ImGui::TreeNodeEx("Model",
                        ImGuiTreeNodeFlags_CollapsingHeader |
                        ImGuiTreeNodeFlags_DefaultOpen)) {
    const float button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 3.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
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

    draw_slider("intensity", "Intensity", nullptr,
                &pass.intensity, 0.0f, 2.0f, 1.0f);
    draw_slider("local_tone", "Local Tone", nullptr,
                &pass.local_tone, 0.0f, 2.0f, 1.0f);
    draw_slider("local_structure", "Local Structure", nullptr,
                &pass.local_structure, 0.0f, 2.0f, 1.0f);

    // Skin Structure: checkbox on a fresh line, caption and slider to its
    // right. The checkbox is emitted first (naturally on its own line), then
    // SameLine() lays out the rest of the row.
    ImGui::AlignTextToFramePadding();
    changed |= ImGui::Checkbox("##skin_structure_enabled", &pass.auto_mask);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Enables the model's character skin mask and adjusts its strength.");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Skin Structure");
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 28.0f);
    if (!pass.auto_mask) ImGui::BeginDisabled();
    changed |= ImGui::SliderFloat("##skin_structure_value",
                                  &pass.skin_structure,
                                  0.0f, 1.0f, "%.2f");
    if (!pass.auto_mask) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!pass.auto_mask) ImGui::BeginDisabled();
    if (ImGui::Button("##skin_structure_reset", ImVec2(24.0f, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
    if (!pass.auto_mask) ImGui::EndDisabled();
  }

  // --- Dynamic shader param sections (one per category) ---
  uint32_t i = 0;
  while (i < kShaderParamCount) {
    const char* category = kShaderParams[i].category;

    uint32_t next = i;
    while (next < kShaderParamCount &&
           strcmp(kShaderParams[next].category, category) == 0) {
      ++next;
    }

    ImGui::Separator();
    const bool open = ImGui::TreeNodeEx(
        category,
        ImGuiTreeNodeFlags_CollapsingHeader |
        ImGuiTreeNodeFlags_DefaultOpen);

    if (open) {
      for (uint32_t j = i; j < next; ++j) {
        const auto& param = kShaderParams[j];
        const std::string id =
            std::string(category) + "_" + param.name;

        if (param.is_checkbox) {
          bool checked = pass.params[j] > 0.5f;
          const std::string cb_label =
              std::string(param.name) + "###param_" + std::to_string(j);
          changed |= ImGui::Checkbox(cb_label.c_str(), &checked);
          if (ImGui::IsItemHovered() && param.tooltip != nullptr) {
            ImGui::SetTooltip("%s", param.tooltip);
          }
          pass.params[j] = checked ? 1.0f : 0.0f;
        } else {
          draw_slider(id.c_str(), param.name, param.tooltip,
                      &pass.params[j], param.min_value,
                      param.max_value, param.default_value);
        }
      }
    }

    i = next;
  }

  // --- Advanced ---
  ImGui::Separator();
  if (ImGui::TreeNodeEx("Advanced",
                        ImGuiTreeNodeFlags_CollapsingHeader)) {
    ImGui::AlignTextToFramePadding();
    changed |= ImGui::Checkbox("UI Correction", &pass.ui_correction);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Experimental. Enables the model's UI correction pass.");
    }
  }

  if (!enabled) {
    ImGui::EndDisabled();
  }

  ImGui::PopID();
}

}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  bool changed = false;

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  changed |= ImGui::Checkbox("Require DLSS",
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
      if (ImGui::BeginTabItem(label)) {
        // The open tab determines which pass's debug view is rendered.
        settings.active_pass = static_cast<int>(i);
        DrawPassSettingsBody(i, settings, settings.pass_enabled[i], changed);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

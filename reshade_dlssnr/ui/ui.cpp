#include <imgui.h>

#include "ui.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "addon_controller.h"
#include "compute/shader_params.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "settings.h"
#include "version.h"

namespace reshade_dlssnr {

namespace {

constexpr float kControlLabelWidth = 220.0f;
constexpr float kResetButtonWidth = 24.0f;

// ---------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------

// Runs `fn` inside a disabled region when `disabled` is true.
template <typename Fn>
void WithDisabled(bool disabled, Fn&& fn) {
  if (disabled) ImGui::BeginDisabled();
  fn();
  if (disabled) ImGui::EndDisabled();
}

// A float slider row: label at the control column, slider filling the
// remaining width, reset button at the far right, and (for passes > 0) an
// inherit toggle after reset. When inheriting, the slider is disabled and
// shows the value resolved from earlier passes.
bool DrawFloatRow(uint32_t pass_index,
                  const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  bool* inherit_flag,
                  float inherited_value,
                  float lo,
                  float hi,
                  float default_value) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = (pass_index > 0) && (inherit_flag != nullptr);
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - trailing_width -
                          ImGui::GetStyle().ItemSpacing.x);

  const bool inheriting = has_inherit && *inherit_flag;
  float display_value = inheriting ? inherited_value : *raw_value;

  const std::string slider_id = std::string("##") + id;
  bool slider_changed = false;
  WithDisabled(inheriting, [&] {
    slider_changed =
        ImGui::SliderFloat(slider_id.c_str(), &display_value, lo, hi, "%.2f");
  });
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
  if (slider_changed && !inheriting) {
    *raw_value = display_value;
    changed = true;
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *raw_value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reset to default");
  }
  ImGui::PopID();

  if (has_inherit) {
    ImGui::SameLine();
    const std::string inherit_id = std::string(id) + "_inherit";
    ImGui::PushID(inherit_id.c_str());
    const bool on = *inherit_flag;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      *inherit_flag = !*inherit_flag;
      changed = true;
    }
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Inherit this value from the previous pass");
    }
    ImGui::PopID();
  }

  return changed;
}

// A checkbox placed at the label column (used for standalone checkboxes and
// as the left half of composite rows).
bool DrawCheckboxAtLabel(const char* id, bool* value) {
  const std::string cb_id = std::string("##") + id;
  return ImGui::Checkbox(cb_id.c_str(), value);
}

// A combo row with per-item tooltips. `tooltips` is an array of item_count
// pointers (nullptr entries are fine) and `preview_tooltips` works the same
// way for the closed combo's hover tooltip.
bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  const char* const* item_tooltips,
                  int item_count) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);

  const int clamped = (*value >= 0 && *value < item_count) ? *value : 0;
  const char* preview = items[clamped];
  const std::string combo_id = std::string("##") + id;

  bool changed = false;
  if (ImGui::BeginCombo(combo_id.c_str(), preview)) {
    for (int i = 0; i < item_count; ++i) {
      const bool selected = (i == clamped);
      if (ImGui::Selectable(items[i], selected)) {
        if (*value != i) {
          *value = i;
          changed = true;
        }
      }
      if (ImGui::IsItemHovered() && item_tooltips != nullptr &&
          item_tooltips[i] != nullptr) {
        ImGui::SetTooltip("%s", item_tooltips[i]);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  // Hovering the closed combo shows the tooltip of the current selection.
  if (ImGui::IsItemHovered() && item_tooltips != nullptr &&
      item_tooltips[clamped] != nullptr) {
    ImGui::SetTooltip("%s", item_tooltips[clamped]);
  }

  return changed;
}

// ---------------------------------------------------------------------------
// Section: Target
// ---------------------------------------------------------------------------

void DrawTargetSection(Settings& settings, uint32_t pass_index,
                       bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  // A pass can be at DLSS Output only if every earlier pass is too.
  bool can_be_at_dlss = true;
  for (uint32_t j = 0; j < pass_index; ++j) {
    if (settings.passes[j].injection_stage == InjectionStage::Present) {
      can_be_at_dlss = false;
      break;
    }
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Where the model pipeline runs for this pass. DLSS Output writes "
        "directly into the game's DLSS result (before UI and tone mapping); "
        "Backbuffer runs at the ReShade present event on the final frame.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const float button_width =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) /
      2.0f;

  const bool at_dlss = pass.injection_stage == InjectionStage::PostDLSS;
  const bool at_present = pass.injection_stage == InjectionStage::Present;

  WithDisabled(!can_be_at_dlss, [&] {
    if (at_dlss) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("DLSS Output", ImVec2(button_width, 0.0f)) &&
        can_be_at_dlss && !at_dlss) {
      pass.injection_stage = InjectionStage::PostDLSS;
      changed = true;
    }
    if (at_dlss) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Run inline after the game's DLSS upscaling, before UI. "
          "Only available when all earlier passes also target DLSS Output.");
    }
  });

  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

  if (at_present) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  if (ImGui::Button("Backbuffer", ImVec2(button_width, 0.0f)) && !at_present) {
    pass.injection_stage = InjectionStage::Present;
    // Once a pass targets the backbuffer, all later passes must too.
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
  }
  if (at_present) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Run at the ReShade present event on the final frame. "
        "Any later pass is forced to Backbuffer as well.");
  }
}

// ---------------------------------------------------------------------------
// Section: Pipeline (Working Scale)
// ---------------------------------------------------------------------------

void DrawPipelineSection(uint32_t pass_index,
                         Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  changed |= DrawFloatRow(
      pass_index, "working_scale", "Working Scale",
      "Model evaluation resolution as a fraction of the frame. "
      "1.0 runs the model at full resolution.",
      &pass.working_scale, &pass.inherit_working_scale,
      resolved.working_scale, 0.25f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Section: Debug view (shared across passes)
// ---------------------------------------------------------------------------

void DrawSharedDebugView(Settings& settings, bool& changed) {
  std::vector<const char*> names;
  std::vector<const char*> tips;
  names.reserve(8);
  tips.reserve(8);

  names.push_back("Off");
  tips.push_back(
      "Disable the debug view and show the normal composed output.");

  for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
    names.push_back(kDebugViews[i].name);
    tips.push_back(kDebugViews[i].tooltip);
  }

  changed |= DrawComboRow(
      "debug_view", "Debug View",
      "Display a pipeline resource instead of the final image. "
      "Shared across all passes; the currently open pass tab renders it.",
      &settings.debug_view, names.data(), tips.data(),
      static_cast<int>(names.size()));
}

// ---------------------------------------------------------------------------
// Section: Model
// ---------------------------------------------------------------------------

bool DrawSkinStructureRow(uint32_t pass_index,
                          PassSettings& pass,
                          const PassSettings& resolved) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("skin_structure_enabled", &pass.auto_mask);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Enables the model's character skin mask and adjusts its strength.");
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("Skin Structure");
  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = pass_index > 0;
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - trailing_width -
                          ImGui::GetStyle().ItemSpacing.x);

  const bool inheriting = has_inherit && pass.inherit_skin_structure;
  const bool slider_disabled = !pass.auto_mask || inheriting;
  float display_value =
      inheriting ? resolved.skin_structure : pass.skin_structure;

  WithDisabled(slider_disabled, [&] {
    if (ImGui::SliderFloat("##skin_structure_value", &display_value,
                           0.0f, 1.0f, "%.2f") && !inheriting) {
      pass.skin_structure = display_value;
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Strength of the model's character skin structure. "
          "Disabled while Character Mask is off.");
    }
  });

  ImGui::SameLine();
  ImGui::PushID("skin_structure_reset");
  WithDisabled(!pass.auto_mask, [&] {
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
  });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reset to default");
  ImGui::PopID();

  if (has_inherit) {
    ImGui::SameLine();
    ImGui::PushID("skin_structure_inherit");
    const bool on = pass.inherit_skin_structure;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.inherit_skin_structure = !pass.inherit_skin_structure;
      changed = true;
    }
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Inherit this value from the previous pass");
    }
    ImGui::PopID();
  }

  return changed;
}

void DrawModelSection(uint32_t pass_index,
                      Settings& settings,
                      bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  if (!ImGui::TreeNodeEx("Model",
                         ImGuiTreeNodeFlags_CollapsingHeader |
                             ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  // Preset row.
  {
    const float button_width =
        (ImGui::GetContentRegionAvail().x - kControlLabelWidth) / 3.0f -
        ImGui::GetStyle().ItemSpacing.x;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Selects one of the model's trained presets.");
    }
    ImGui::SameLine(kControlLabelWidth);

    static const char* kPresetNames[] = {"A", "B", "C"};
    static const char* kPresetTooltips[] = {
        "Select model preset A.",
        "Select model preset B.",
        "Select model preset C.",
    };
    for (int i = 0; i < 3; ++i) {
      if (i > 0) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);

      const bool selected = pass.preset == i;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(kPresetNames[i], ImVec2(button_width, 0.0f)) &&
          pass.preset != i) {
        pass.preset = i;
        changed = true;
      }
      if (selected) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", kPresetTooltips[i]);
      }
    }
  }

  changed |= DrawFloatRow(
      pass_index, "intensity", "Intensity",
      "Overall strength of the model's edit.",
      &pass.intensity, &pass.inherit_intensity, resolved.intensity,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_tone", "Local Tone",
      "Strength of the model's local tone adjustments.",
      &pass.local_tone, &pass.inherit_local_tone, resolved.local_tone,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_structure", "Local Structure",
      "Strength of the model's local structure detail.",
      &pass.local_structure, &pass.inherit_local_structure,
      resolved.local_structure, 0.0f, 2.0f, 1.0f);

  changed |= DrawSkinStructureRow(pass_index, pass, resolved);
}

// ---------------------------------------------------------------------------
// Section: Dynamic shader parameters
// ---------------------------------------------------------------------------

int FindShaderParamIndex(const char* name) {
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    if (strcmp(kShaderParams[i].name, name) == 0) return static_cast<int>(i);
  }
  return -1;
}

void DrawShaderParamSections(uint32_t pass_index,
                             Settings& settings,
                             bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

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
        category, ImGuiTreeNodeFlags_CollapsingHeader |
                      ImGuiTreeNodeFlags_DefaultOpen);

    if (open) {
      for (uint32_t j = i; j < next; ++j) {
        const auto& param = kShaderParams[j];
        const std::string id = std::string(category) + "_" + param.name;

        // Dependency gate: if the gating parameter is off, disable this one.
        bool dependency_off = false;
        if (param.depends_on != nullptr) {
          const int gate = FindShaderParamIndex(param.depends_on);
          if (gate >= 0 && pass.params[gate] < 0.5f) dependency_off = true;
        }

        WithDisabled(dependency_off, [&] {
          if (param.is_checkbox) {
            bool checked = pass.params[j] > 0.5f;
            ImGui::AlignTextToFramePadding();
            changed |= DrawCheckboxAtLabel(id.c_str(), &checked);
            if (ImGui::IsItemHovered() && param.tooltip != nullptr) {
              ImGui::SetTooltip("%s", param.tooltip);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(param.name);
            pass.params[j] = checked ? 1.0f : 0.0f;
          } else {
            changed |= DrawFloatRow(
                pass_index, id.c_str(), param.name, param.tooltip,
                &pass.params[j], &pass.inherit_params[j], resolved.params[j],
                param.min_value, param.max_value, param.default_value);
          }
        });
      }
    }

    i = next;
  }
}

// ---------------------------------------------------------------------------
// Section: Advanced
// ---------------------------------------------------------------------------

void DrawAdvancedSection(PassSettings& pass, bool& changed) {
  if (!ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_CollapsingHeader)) {
    return;
  }

  ImGui::AlignTextToFramePadding();
  changed |= ImGui::Checkbox("UI Correction", &pass.ui_correction);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Experimental. Enables the model's UI correction pass.");
  }
}

// ---------------------------------------------------------------------------
// Pass tab body
// ---------------------------------------------------------------------------

void DrawPassSettingsBody(uint32_t pass_index,
                          Settings& settings,
                          bool enabled,
                          bool& changed) {
  ImGui::PushID(static_cast<int>(pass_index));
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawModelSection(pass_index, settings, changed);
    DrawShaderParamSections(pass_index, settings, changed);
    ImGui::Separator();
    DrawAdvancedSection(settings.passes[pass_index], changed);
  });
  ImGui::PopID();
}

}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  bool changed = false;

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Master switch. When off, no passes run and no GPU work is done.");
  }

  changed |= ImGui::Checkbox("Require DLSS", &settings.only_super_sampling);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Only run when the game is using DLSS SuperSampling. "
        "Turn off to run on any frame.");
  }

  if (settings.enabled) {
    for (uint32_t i = 1; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Enable Pass %u", i + 1);
      changed |= ImGui::Checkbox(label, &settings.pass_enabled[i]);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Toggle this pass. When off its tab is greyed out but still "
            "clickable.");
      }
    }
  }

  ImGui::Separator();

  // --- Debug view (shared across passes) ---
  DrawSharedDebugView(settings, changed);

  // --- HDR section (shared across passes) ---
  if (ImGui::TreeNodeEx("HDR",
                        ImGuiTreeNodeFlags_CollapsingHeader)) {
    changed |= ImGui::Checkbox("Enable HDR Handling",
                               &settings.hdr_enable);
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          "Normalize HDR input for the model. Only affects passes running "
          "at DLSS Output. The backbuffer path is already display-referred.");
    }

    const bool hdr_on = settings.hdr_enable;

    // Live measurement feedback. Without this the HDR stage has no visible
    // effect during Phase A (measurement only, no tonemapping yet).
    if (hdr_on) {
      const HdrSnapshot snap = AddonController::Get().GetHdrSnapshot();

      const char* format_name = "SDR";
      switch (snap.resolved_format) {
        case HdrFormat::Auto: format_name = "Auto"; break;
        case HdrFormat::Sdr:  format_name = "SDR"; break;
        case HdrFormat::ScRgb: format_name = "scRGB"; break;
        case HdrFormat::Hdr10: format_name = "HDR10"; break;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Status");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Live state of the HDR measurement. Phase A only measures "
            "the scene; the exposure and white point will drive the "
            "tonemap in a later phase.");
      }
      ImGui::SameLine(kControlLabelWidth);

      if (!snap.ready) {
        ImGui::TextDisabled("waiting for first measurement...");
      } else {
        ImGui::Text("Format: %s   Exposure: %.3f   White: %.3f",
                    format_name, snap.exposure, snap.white_point);
      }
    }

    WithDisabled(!hdr_on, [&] {
      // Format combo.
      {
        static const char* kFormatNames[] = {
            "Auto", "SDR", "scRGB", "HDR10"};
        static const char* kFormatTips[] = {
            "Detect from the DLSS output's DXGI format.",
            "Force SDR sRGB interpretation.",
            "Force linear scRGB (1.0 = 80 nits).",
            "Force HDR10 (PQ ST.2084)."};
        int fmt = static_cast<int>(settings.hdr_format);
        changed |= DrawComboRow("hdr_format", "Format",
                                "How the source image is encoded. "
                                "Auto detects from the DXGI format.",
                                &fmt, kFormatNames, kFormatTips, 4);
        settings.hdr_format = static_cast<HdrFormat>(fmt);
      }

      // Chroma edit combo.
      {
        static const char* kChromaNames[] = {"Additive", "Ratio"};
        static const char* kChromaTips[] = {
            "Apply the model's edit as an RGB delta in the SDR intermediate, "
            "then inverse-tonemap back to HDR. Predictable; the edit's "
            "magnitude is bounded by the SDR delta. Default.",
            "Apply the model's edit as a per-channel multiplicative ratio "
            "in linear space. Preserves relative brightness changes exactly, "
            "but can amplify noise where the source is near zero."};
        int mode = static_cast<int>(settings.hdr_chroma_mode);
        changed |= DrawComboRow(
            "hdr_chroma_mode", "Chroma Edit",
            "How the model's chroma edit is transferred back to HDR.",
            &mode, kChromaNames, kChromaTips, 2);
        settings.hdr_chroma_mode = static_cast<ChromaEditMode>(mode);
      }

      changed |= DrawFloatRow(
          0, "hdr_responsiveness", "Responsiveness",
          "How quickly the exposure follows scene changes. "
          "Lower = slower and cheaper (measures less often); "
          "higher = faster and more expensive (measures every frame).",
          &settings.hdr_responsiveness, nullptr, 0.0f,
          0.0f, 1.0f, 0.50f);

      changed |= DrawFloatRow(
          0, "hdr_midtone", "Midtone Target",
          "Where the scene's median luminance lands in SDR. "
          "18% grey is the classic choice.",
          &settings.hdr_midtone, nullptr, 0.0f,
          0.05f, 0.50f, 0.18f);

      changed |= DrawFloatRow(
          0, "hdr_white_percentile", "White Percentile",
          "Histogram percentile used as the white point. Higher values "
          "preserve more highlight headroom; lower values roll off sooner.",
          &settings.hdr_white_percentile, nullptr, 0.0f,
          0.90f, 0.999f, 0.99f);

      changed |= DrawFloatRow(
          0, "hdr_highlight_headroom", "Highlight Headroom",
          "Multiplier on the auto-detected white point. 1.0 uses the "
          "histogram's own value and clips the top 1% of pixels, which "
          "flattens highlights on the round trip. 8.0 (default) gives "
          "generous headroom so highlights survive intact. Lower values "
          "compress the highlights harder.",
          &settings.hdr_highlight_headroom, nullptr, 0.0f,
          0.25f, 16.0f, 8.0f);

      changed |= DrawFloatRow(
          0, "hdr_edit_strength", "Edit Strength",
          "Global multiplier on the model's edit before HDR remap.",
          &settings.hdr_edit_strength, nullptr, 0.0f,
          0.0f, 2.0f, 1.0f);
    });

    // Diagnostics: raw measurement values, for scenes where the exposure
    // converges somewhere unexpected. Everything below is read-only.
    if (hdr_on) {
      if (ImGui::TreeNodeEx("Diagnostics",
                            ImGuiTreeNodeFlags_CollapsingHeader)) {
        const HdrSnapshot snap = AddonController::Get().GetHdrSnapshot();

        if (!snap.has_histogram) {
          ImGui::TextDisabled("Waiting for first histogram measurement...");
        } else {
          ImGui::Text("Samples: %u", snap.samples);
          ImGui::Text("Scene median (P50): %.4f", snap.p50_luma);
          ImGui::Text("White point source (P%.1f): %.4f",
                      settings.hdr_white_percentile * 100.0f,
                      snap.p_hi_luma);
          ImGui::Text("Target exposure: %.3f   Current: %.3f",
                      snap.target_exposure, snap.exposure);
          ImGui::Text("White point (post-headroom): %.3f",
                      snap.white_point *
                          std::max(settings.hdr_highlight_headroom, 0.01f));
          ImGui::Text("Frames since last measurement: %u",
                      snap.frames_since_measure);

          ImGui::Separator();
          ImGui::TextUnformatted("Histogram (log2 luminance)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Log2 luminance distribution of the measured source. "
                "Left = dark, right = bright. The scene's exposure is set "
                "so the median lands at the Midtone Target; the white "
                "point comes from the selected percentile.");
          }

          float bins_float[256];
          float peak = 1.0f;
          for (int i = 0; i < 256; ++i) {
            bins_float[i] = static_cast<float>(snap.histogram[i]);
            if (bins_float[i] > peak) peak = bins_float[i];
          }

          ImGui::PlotHistogram("##hdr_histogram", bins_float, 256,
                               0, nullptr, 0.0f, peak,
                               ImVec2(-1.0f, 80.0f));
        }
      }
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

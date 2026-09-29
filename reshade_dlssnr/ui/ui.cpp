#include <imgui.h>

#include "ui.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "addon_controller.h"
#include "compute/shader_params.h"
#include "settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"
#include "version.h"

namespace reshade_dlssnr {

// Set by ApplyPassReorder and consumed by DrawOverlay on the next frame. Tab
// IDs are positional, so after a reorder ImGui still has the tab at the old
// position selected and would overwrite active_pass back to it. This forces
// the tab that now holds the active pass to be selected instead.
int g_force_select_tab = -1;

// A move queued by the order buttons. Applied at the top of the next frame
// rather than in place: the buttons are drawn inside the tab bar loop, and
// permuting the array mid-loop redraws the tabs to the right with the new
// names and leaves the current tab's body half-drawn for one frame.
enum class PendingMove { None, Left, Right };
PendingMove g_pending_move = PendingMove::None;
int g_pending_move_index = -1;

// Defined below, but used by DrawPassSettingsBody's move buttons.
void ApplyPassReorder(Settings& settings, const int* order, bool& changed);

namespace {


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

  static const char* kTargetLabels[] = {"DLSS Output", "Backbuffer"};
  static const char* kTargetTips[] = {
      "Run this pass on the DLSS upscaled image, before the game applies "
      "UI and tone mapping.\n"
      "Unavailable when an earlier pass targets the Backbuffer, because "
      "passes run in order and later stages cannot reach back to earlier "
      "ones.",
      "Run this pass on the final frame, after every other effect "
      "including UI and post-processing.\n"
      "Selecting this forces all later passes to Backbuffer as well, "
      "because they cannot run earlier than this one."};
  const bool kTargetDisabled[] = {!can_be_at_dlss, false};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  if (ImGui::IsItemHovered()) {
    Tip(
        "Where this pass runs in the frame.\n"
        "  DLSS Output — runs right after the game's DLSS upscaling, "
        "before UI and tone mapping are applied. Recommended: this is "
        "the cleanest image the model can see.\n"
        "  Backbuffer — runs at the very end of the frame, on the "
        "finished image.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const int selected =
      pass.injection_stage == InjectionStage::PostDLSS ? 0 : 1;
  const int clicked = DrawGroupButtons("target", kTargetLabels, kTargetTips,
                                       2, selected, kTargetDisabled);

  if (clicked == 0) {
    pass.injection_stage = InjectionStage::PostDLSS;
    changed = true;
  } else if (clicked == 1) {
    pass.injection_stage = InjectionStage::Present;
    // Once a pass targets the backbuffer, all later passes must too.
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
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
      "The resolution the model runs at, as a fraction of the frame.\n"
      "Lower values run the model faster but with less detail. "
      "1.0 runs at full resolution (slowest, best quality).",
      &pass.working_scale, &pass.inherit_working_scale,
      resolved.working_scale, 0.10f, 1.0f, 1.0f);
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
    Tip(
        "Lets the model detect character skin and apply its edits to it "
        "separately, so faces and hands are handled more gently than "
        "the rest of the scene.");
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
      Tip(
          "How strongly the model's edits are applied to detected skin.\n"
          "Only available when the checkbox to the left is enabled.");
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
  if (ImGui::IsItemHovered()) Tip("Reset to default");
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
      Tip("Inherit this value from the previous pass");
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

  // Indent the whole sub-section (header included), so it reads as nested
  // inside the pass tab it belongs to.
  IndentScope indent;

  if (!PersistentSubHeader("Model", settings.ui_model_open, changed)) {
    return;
  }

  // Preset row.
  {
    static const char* kPresetNames[] = {"A", "B", "C"};
    static const char* kPresetTooltips[] = {
        "Model preset A.",
        "Model preset B.",
        "Model preset C.",
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    if (ImGui::IsItemHovered()) {
      Tip(
          "Which of the model's three trained presets to use. "
          "They differ in how strongly the model's edits are applied.");
    }
    ImGui::SameLine(kControlLabelWidth);

    const int clicked = DrawGroupButtons(
        "preset", kPresetNames, kPresetTooltips, 3, pass.preset, nullptr);
    if (clicked >= 0) {
      pass.preset = clicked;
      changed = true;
    }
  }

  changed |= DrawFloatRow(
      pass_index, "intensity", "Intensity",
      "How strongly the model is instructed to edit the image.\n"
      "0 turns the model's edits off entirely (only the rest of the "
      "pipeline still runs). 1 is the model's intended default.",
      &pass.intensity, &pass.inherit_intensity, resolved.intensity,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_tone", "Local Tone",
      "How much of the model's local tone adjustments to apply.\n"
      "Lower values keep more of the game's original grading.",
      &pass.local_tone, &pass.inherit_local_tone, resolved.local_tone,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_structure", "Local Structure",
      "How much of the model's local structure detail to apply.\n"
      "Lower values keep the game's original texture character.",
      &pass.local_structure, &pass.inherit_local_structure,
      resolved.local_structure, 0.0f, 2.0f, 1.0f);

  changed |= DrawSkinStructureRow(pass_index, pass, resolved);

  // UI Correction sits under Skin Structure: both are model-side masks
  // that affect what the edit preserves. Same checkbox-at-label style, so
  // the two read as siblings.
  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("ui_correction", &pass.ui_correction);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Asks the model to reduce its edits where it detects UI elements, "
        "so menus and HUD text stay crisp.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
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
  uint32_t category_index = 0;
  while (i < kShaderParamCount) {
    const char* category = kShaderParams[i].category;

    uint32_t next = i;
    while (next < kShaderParamCount &&
           strcmp(kShaderParams[next].category, category) == 0) {
      ++next;
    }

    ImGui::Separator();
    // Category order is stable in kShaderParams, so the per-category UI
    // state stays mapped correctly. If the table is reordered, entries
    // past kShaderParamCount are ignored and new ones default to open.
    bool& category_open =
        settings.ui_category_open[category_index < 16 ? category_index : 15];

    // Sub-section: same indent-and-dim treatment as "Model", so categories
    // read as sitting one level in from the pass tab's own content.
    IndentScope indent;
    const bool open = PersistentSubHeader(category, category_open, changed);
    category_index++;

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
              Tip("%s", param.tooltip);
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
// Pass tab body
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Section: Residual edit
// ---------------------------------------------------------------------------
//
// How this pass's model edit is carried onto the source image. Per pass,
// because the edit is a statement about what this pass's model produced.
//
// This used to sit in the shared HDR section as "Chroma Edit" and "Edit
// Strength", which was wrong twice over: it is not HDR-specific (the transfer
// runs whether or not HDR handling is on), and it is not global.
//
// No enable checkbox. 0 is a pass-through and 1 is the model's own magnitude,
// so the slider already has both an off position and a neutral one; a
// checkbox on top would only duplicate the 0 end of it.
void DrawResidualSection(uint32_t pass_index,
                         Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Residual Edit", settings.ui_residual_open,
                           changed)) {
    return;
  }

  changed |= DrawFloatRow(
      pass_index, "edit_strength", "Edit Strength",
      "Scales the model's edit before it is carried onto the source.\n"
      "0 makes the compose a pass-through: the model still runs, its edit is "
      "just not applied. 1 applies the edit at the magnitude the model "
      "produced. Above 1 amplifies it.\n"
      "The edit is applied as a per-channel ratio, which is scale-invariant, "
      "so this behaves the same whether or not HDR handling is on.",
      &pass.residual.edit_strength, nullptr, 0.0f, 0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_chroma", "Chroma Strength",
      "How much of the edit's chroma lands relative to its brightness.\n"
      "1 keeps the model's per-channel edit as it is. 0 leaves only the "
      "brightness change and takes hue out of the edit entirely, which is "
      "the setting for a scene where the model's chroma shift reads as "
      "wrong.",
      &pass.residual.chroma_strength, nullptr, 1.0f, 0.0f, 1.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_gamma", "Edit Gamma",
      "Shapes the edit's response curve.\n"
      "Below 1 amplifies small edits, above 1 compresses them. 0 magnitude "
      "always stays 0, so this only changes how strongly a given edit "
      "lands, not whether it lands.",
      &pass.residual.edit_gamma, nullptr, 1.0f, 0.25f, 4.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_highlight", "Highlight Protect",
      "Fades the edit out above mid-to-high brightness.\n"
      "At 1 the edit is removed entirely at white. Useful when the model's "
      "brightness lift blows highlights the source image kept under "
      "control.",
      &pass.residual.highlight_protect, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_shadow", "Shadow Protect",
      "Fades the edit out below mid-to-low brightness.\n"
      "At 1 the edit is removed entirely at black. Useful when the model's "
      "darkening crushes shadow detail, or when its brightening turns near-"
      "black noise into visible patches.",
      &pass.residual.shadow_protect, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_eps", "Ratio Eps",
      "Denominator floor for the per-channel ratio. Higher values make "
      "near-black pixels contribute less to the edit.",
      &pass.residual.ratio_eps, nullptr, 1e-2f, 1e-6f, 0.5f, 1e-2f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_brighten", "Ratio Brighten",
      "Upper bound on how much the model can brighten a channel. "
      "Soft-clipped: approached, never reached.",
      &pass.residual.ratio_brighten, nullptr, 64.0f, 1.0f, 256.0f, 64.0f);

  changed |= DrawFloatRow(
      pass_index, "residual_ratio_darken", "Ratio Darken",
      "Lower bound on how much the model can darken a channel. "
      "Soft-clipped: approached, never reached.",
      &pass.residual.ratio_darken, nullptr, 1.0f / 16.0f,
      1.0f / 256.0f, 1.0f, 1.0f / 16.0f);
}

void DrawRcasSection(uint32_t pass_index,
                     Settings& settings,
                     bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Sharpening", settings.ui_rcas_open, changed)) {
    return;
  }

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("rcas_enabled", &pass.rcas_enabled);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Sharpen the model's upsampled output with FSR RCAS.\n"
        "Applies after the working-scale images are upsampled, so it "
        "restores some of the crispness lost at lower working scales.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("Enable RCAS");

  WithDisabled(!pass.rcas_enabled, [&] {
    changed |= DrawFloatRow(
        pass_index, "rcas_strength", "Strength",
        "How aggressively to sharpen.\n"
        "RCAS limits the amount it can sharpen each pixel by the local "
        "contrast, so this raises the ceiling rather than applying a "
        "uniform sharpening: flat areas stay flat and edges gain "
        "definition.",
        &pass.rcas_strength, nullptr, 0.5f, 0.0f, 1.0f, 0.5f);
  });
}

void DrawPassSettingsBody(uint32_t pass_index,
                          Settings& settings,
                          bool& changed) {
  ImGui::PushID(static_cast<int>(pass_index));

  PassSettings& pass = settings.passes[pass_index];

  char label[32];
  std::snprintf(label, sizeof(label), "Enable Pass %u", pass_index + 1);
  changed |= ImGui::Checkbox(label, &settings.pass_enabled[pass_index]);
  if (ImGui::IsItemHovered()) {
    Tip("Turn this pass on or off.");
  }
  ImGui::Separator();

  // Name field
  {
    char name_buffer[64];
    std::snprintf(name_buffer, sizeof(name_buffer), "%s", pass.custom_name);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine(kControlLabelWidth);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                            kResetButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##pass_name", name_buffer, sizeof(name_buffer))) {
      std::snprintf(pass.custom_name, sizeof(pass.custom_name), "%s",
                    name_buffer);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.custom_name[0] = '\0';
      changed = true;
    }
    if (ImGui::IsItemHovered()) Tip("Reset to default name");
  }

  // Move buttons
  {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Order");
    ImGui::SameLine(kControlLabelWidth);

    const bool can_move_left = pass_index > 0;
    const bool can_move_right = pass_index + 1 < kMaxPasses;

    WithDisabled(!can_move_left, [&] {
      if (ImGui::Button("<", ImVec2(30, 0)) && can_move_left) {
        g_pending_move = PendingMove::Left;
        g_pending_move_index = static_cast<int>(pass_index);
      }
      if (ImGui::IsItemHovered()) Tip("Move this pass earlier in the pipeline");
    });

    ImGui::SameLine();

    WithDisabled(!can_move_right, [&] {
      if (ImGui::Button(">", ImVec2(30, 0)) && can_move_right) {
        g_pending_move = PendingMove::Right;
        g_pending_move_index = static_cast<int>(pass_index);
      }
      if (ImGui::IsItemHovered()) Tip("Move this pass later in the pipeline");
    });
  }

  const bool enabled = settings.pass_enabled[pass_index];
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawModelSection(pass_index, settings, changed);
    DrawShaderParamSections(pass_index, settings, changed);
    DrawResidualSection(pass_index, settings, changed);
    DrawRcasSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawGradeSection(
        "grade_input", "Grade Model Input",
        "Grade the image the model reads.\n"
        "The model edits whatever it is given, so this changes what it "
        "produces as well as what it sees.",
        settings.passes[pass_index].grade_input, changed);
    DrawGradeSection(
        "grade_output", "Grade Model Output",
        "Grade the image the model produced, before it is published.\n"
        "This is the model's result with a grade on top; it does not feed "
        "back into the model.",
        settings.passes[pass_index].grade_output, changed);
  });
  ImGui::PopID();
}

}  // namespace

// Permutes settings.passes, settings.pass_enabled and settings.active_pass so
// position i holds the pass that was at order[i]. Re-applies what the pass
// body assumes about positions: pass 0 is the inheritance root and has no
// flags, and a Present pass forces every later pass to Present as well.
void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  PassSettings new_passes[kMaxPasses];
  bool new_enabled[kMaxPasses];
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    new_passes[i] = settings.passes[order[i]];
    new_enabled[i] = settings.pass_enabled[order[i]];
  }
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    settings.passes[i] = new_passes[i];
    settings.pass_enabled[i] = new_enabled[i];
  }

  // active_pass names a pass, not a position, so it follows its data.
  const int old_active = settings.active_pass;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (order[i] == old_active) {
      settings.active_pass = static_cast<int>(i);
      break;
    }
  }

  settings.passes[0].inherit_working_scale = false;
  settings.passes[0].inherit_intensity = false;
  settings.passes[0].inherit_local_tone = false;
  settings.passes[0].inherit_local_structure = false;
  settings.passes[0].inherit_skin_structure = false;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    settings.passes[0].inherit_params[i] = false;
  }

  for (uint32_t i = 1; i < kMaxPasses; ++i) {
    if (settings.passes[i - 1].injection_stage == InjectionStage::Present) {
      settings.passes[i].injection_stage = InjectionStage::Present;
    }
  }

  g_force_select_tab = settings.active_pass;
  changed = true;
}

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  g_show_tooltips = settings.show_tooltips;
  bool changed = false;

  // A move queued by last frame's order buttons. Applied before anything is
  // drawn, so this frame sees the new order from its first widget.
  if (g_pending_move != PendingMove::None && g_pending_move_index >= 0 &&
      g_pending_move_index < static_cast<int>(kMaxPasses)) {
    const int idx = g_pending_move_index;
    int order[kMaxPasses];
    for (uint32_t i = 0; i < kMaxPasses; ++i) order[i] = static_cast<int>(i);
    if (g_pending_move == PendingMove::Left && idx > 0) {
      std::swap(order[idx - 1], order[idx]);
    } else if (g_pending_move == PendingMove::Right &&
               idx + 1 < static_cast<int>(kMaxPasses)) {
      std::swap(order[idx], order[idx + 1]);
    }
    g_pending_move = PendingMove::None;
    ApplyPassReorder(settings, order, changed);

    // SaveSettings calls RecomputeResolved, and the resolved view is what the
    // inherited-value rows below read. Without this the frame of the move
    // shows the old predecessors; every frame after it, when the end-of-frame
    // save runs, shows the new ones. Persist here so the first frame agrees
    // with the rest.
    SaveSettings();
    changed = false;
  }

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  const bool master_hovered = ImGui::IsItemHovered();

  // Show Tooltips checkbox, right-aligned on the same row as Enable
  // DLSSNR.
  {
    const char* kTipLabel = "Show Tooltips";
    const float checkbox_w =
        ImGui::GetFrameHeight() +
        ImGui::GetStyle().ItemInnerSpacing.x +
        ImGui::CalcTextSize(kTipLabel).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetContentRegionAvail().x - checkbox_w);
    changed |= ImGui::Checkbox(kTipLabel, &settings.show_tooltips);
    if (ImGui::IsItemHovered()) {
      Tip("Show tooltips for controls in this panel.");
    }
  }

  if (master_hovered) {
    Tip(
        "Master switch. When off, no passes run and no GPU work is done.");
  }

  changed |= ImGui::Checkbox("Require DLSS", &settings.only_super_sampling);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Only run when the game is actively using DLSS upscaling.\n"
        "Turn this off to also run on frames where DLSS is not in use. "
        "Doing so is rarely useful: the model expects a DLSS result as "
        "its input.");
  }

  SectionDivider();

  // --- Debug view (shared across passes) ---
  DrawSharedDebugView(settings, changed);

  SectionDivider();

  // --- Status (shared across passes) ---
  DrawStatusSection(settings, changed);

  // HDR handling only applies to passes that target DLSS Output. The
  // section is shared, so it is live whenever any enabled pass would use
  // it — not just the currently-selected tab.
  const bool hdr_relevant = AnyPassUsesDlssOutput(settings);

  SectionDivider();

  // The tab bar is visual only. BeginTabItem's return tracks ImGui's own
  // selection, which after a reorder still names the old position - a
  // position that now holds a different pass - so using it to set active_pass
  // would draw the wrong pass for the frame after a move. active_pass is set
  // from clicks instead, and the body is drawn once below it.
  if (ImGui::BeginTabBar("PassTabs")) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      // ### rather than ##: ImHashStr resets at ###, so the ID is the suffix
      // and the display name can change without the tab losing its open
      // state. ## would hash the whole label and renaming would recreate
      // the tab.
      char label[96];
      if (settings.passes[i].custom_name[0] != '\0') {
        std::snprintf(label, sizeof(label), "%s###PassTab%u",
                      settings.passes[i].custom_name, i);
      } else {
        std::snprintf(label, sizeof(label), "Pass %u###PassTab%u", i + 1, i);
      }
      const bool force_this = (g_force_select_tab == static_cast<int>(i));
      const ImGuiTabItemFlags tab_flags =
          force_this ? ImGuiTabItemFlags_SetSelected : 0;
      const bool tab_open = ImGui::BeginTabItem(label, nullptr, tab_flags);
      if (ImGui::IsItemClicked()) {
        settings.active_pass = static_cast<int>(i);
      }
      if (tab_open) {
        ImGui::EndTabItem();
      }
    }
    g_force_select_tab = -1;
    ImGui::EndTabBar();
  }

  if (settings.active_pass >= 0 &&
      settings.active_pass < static_cast<int>(kMaxPasses)) {
    DrawPassSettingsBody(static_cast<uint32_t>(settings.active_pass), settings,
                         changed);
  }
  SectionDivider();

  // --- Colour grade (shared across passes) ---
  if (PersistentHeader("Colour Grade", settings.ui_grade_open, changed)) {
    DrawGradeSection(
        "grade_backbuffer", "Grade Backbuffer",
        "Grade the final image on the swapchain backbuffer, after every pass "
        "has run.\n"
        "This is the last step before the game presents, so it applies to "
        "whatever the passes produced. A debug view replaces the backbuffer "
        "wholesale, so it is not graded.",
        settings.grade_backbuffer, changed);
  }
  SectionDivider();


  SectionDivider();

  // --- HDR section (shared across passes) ---
  DrawHdrSection(settings, hdr_relevant, changed);

  SectionDivider();

  // --- Skin detection (shared across passes; DLSS Output only) ---
  DrawSkinDetectSection(settings, changed);

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

#include <imgui.h>

#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gpu/shader_params.h"
#include "hooks/dlss_guides.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"
#include "version.h"

namespace reshade_dlssnr {

// Set by ApplyPassReorder and consumed by DrawOverlay on the next frame. Tab
// IDs are positional, so after a reorder ImGui still has the tab at the old
// position selected and would overwrite active_pass back to it. This forces
// the tab that now holds the active pass to be selected instead.
int g_force_select_tab = -1;

// Permutes settings.passes, settings.pass_enabled and settings.active_pass so
// position i holds the pass that was at order[i]. Pass targets remain attached
// to their pass profiles; runtime execution groups them by target stage.
void ClearPassInheritanceRoot(PassSettings& pass) {
  pass.inherit_working_scale = false;
  pass.inherit_intensity = false;
  pass.inherit_local_tone = false;
  pass.inherit_local_structure = false;
  pass.inherit_skin_structure = false;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    pass.inherit_params[i] = false;
  }
}

void InitializePassDefaults(PassSettings& pass, InjectionStage stage) {
  pass = PassSettings{};
  pass.injection_stage = stage;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    pass.params[i] = kShaderParams[i].default_value;
  }
}

// Permutes only the active pass profiles. Array slots beyond pass_count remain
// inactive and are not part of the user's visible order.
void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  PassSettings new_passes[kMaxPasses];
  bool new_enabled[kMaxPasses] = {};
  for (uint32_t i = 0; i < settings.pass_count; ++i) {
    new_passes[i] = settings.passes[order[i]];
    new_enabled[i] = settings.pass_enabled[order[i]];
  }
  for (uint32_t i = 0; i < settings.pass_count; ++i) {
    settings.passes[i] = new_passes[i];
    settings.pass_enabled[i] = new_enabled[i];
  }

  const int old_active = settings.active_pass;
  for (uint32_t i = 0; i < settings.pass_count; ++i) {
    if (order[i] == old_active) {
      settings.active_pass = static_cast<int>(i);
      break;
    }
  }

  ClearPassInheritanceRoot(settings.passes[0]);
  g_force_select_tab = settings.active_pass;
  changed = true;
}

void AddPass(Settings& settings, bool& changed) {
  if (settings.pass_count >= kMaxPasses) return;
  const uint32_t new_index = settings.pass_count;
  const uint32_t source_index = static_cast<uint32_t>(std::clamp(
      settings.active_pass, 0, static_cast<int>(settings.pass_count) - 1));
  InitializePassDefaults(settings.passes[new_index],
                         settings.passes[source_index].injection_stage);
  settings.pass_enabled[new_index] = true;
  ++settings.pass_count;
  settings.active_pass = static_cast<int>(new_index);
  g_force_select_tab = settings.active_pass;
  changed = true;
}

void RemoveActivePass(Settings& settings, bool& changed) {
  if (settings.pass_count <= 1) return;
  const uint32_t remove_index = static_cast<uint32_t>(std::clamp(
      settings.active_pass, 0, static_cast<int>(settings.pass_count) - 1));
  for (uint32_t i = remove_index; i + 1 < settings.pass_count; ++i) {
    settings.passes[i] = settings.passes[i + 1];
    settings.pass_enabled[i] = settings.pass_enabled[i + 1];
  }

  --settings.pass_count;
  InitializePassDefaults(settings.passes[settings.pass_count],
                         InjectionStage::Backbuffer);
  settings.pass_enabled[settings.pass_count] = false;
  ClearPassInheritanceRoot(settings.passes[0]);
  settings.active_pass = static_cast<int>(std::min(
      remove_index, settings.pass_count - 1));
  g_force_select_tab = settings.active_pass;
  changed = true;
}

void DrawSavedProfilesSection(Settings& settings, bool& changed) {
  if (!PersistentHeader(
          "Presets", settings.ui_presets_open, changed,
          "Save and restore named profiles of the current DLSSNR settings in ReShade.ini.")) {
    return;
  }

  IndentScope content_indent;
  SavedProfileInfo profiles[kMaxSavedProfiles] = {};
  const uint32_t profile_count = GetSavedProfiles(profiles, kMaxSavedProfiles);
  static int pending_delete = -1;
  static bool profile_name_error = false;
  static char new_profile_name[kSavedProfileNameCapacity] = {};

  if (profile_count == 0) {
    ImGui::TextDisabled("No saved presets.");
  }

  for (uint32_t i = 0; i < profile_count && i < kMaxSavedProfiles; ++i) {
    bool deleted_this_frame = false;
    ImGui::PushID(static_cast<int>(i));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(profiles[i].name[0] != '\0'
                               ? profiles[i].name
                               : "(unnamed preset)");
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
      if (LoadSavedProfile(i)) {
        changed = true;
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Load this preset into the active settings and save it as the current ReShade.ini configuration.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      SaveCurrentToProfile(i);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Overwrite this saved preset with the current settings.");
    }
    ImGui::SameLine();
    if (ImGui::Button("x")) {
      pending_delete = pending_delete == static_cast<int>(i)
                           ? -1
                           : static_cast<int>(i);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Remove this preset after confirming.");
    }
    ImGui::PopID();

    if (pending_delete == static_cast<int>(i)) {
      ImGui::Indent();
      ImGui::TextUnformatted("Remove this preset?");
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.12f, 0.12f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.16f, 0.16f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.08f, 0.08f, 1.0f));
      if (ImGui::Button("Yes")) {
        DeleteSavedProfile(i);
        pending_delete = -1;
        deleted_this_frame = true;
      }
      ImGui::PopStyleColor(3);
      if (!deleted_this_frame) {
        ImGui::SameLine();
        if (ImGui::Button("No")) pending_delete = -1;
      }
      ImGui::Unindent();
    }
    if (deleted_this_frame) break;
  }

  if (profile_count >= kMaxSavedProfiles) ImGui::BeginDisabled();
  const float save_button_width = ImGui::CalcTextSize("Save as New Preset").x +
                                  ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(std::max(
      80.0f, ImGui::GetContentRegionAvail().x - save_button_width -
                 ImGui::GetStyle().ItemSpacing.x));
  if (ImGui::InputText("##new_profile_name", new_profile_name,
                       sizeof(new_profile_name))) {
    profile_name_error = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Save as New Preset") && new_profile_name[0] != '\0') {
    if (SaveCurrentAsProfile(new_profile_name)) {
      new_profile_name[0] = '\0';
      pending_delete = -1;
      profile_name_error = false;
    } else {
      profile_name_error = true;
    }
  }
  if (profile_name_error) {
    ImGui::TextDisabled("Preset name is blank, too long, or already in use.");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Save all current DLSSNR settings under a new name. Up to %u presets can be stored in ReShade.ini.", kMaxSavedProfiles);
  }
  if (profile_count >= kMaxSavedProfiles) ImGui::EndDisabled();
}

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  g_show_tooltips = settings.show_tooltips;
  bool changed = false;

  // A move queued by last frame's order buttons. Applied before anything is
  // drawn, so this frame sees the new order from its first widget.
  if (g_pending_move != PendingMove::None && g_pending_move_index >= 0 &&
      g_pending_move_index < static_cast<int>(settings.pass_count)) {
    const int idx = g_pending_move_index;
    int order[kMaxPasses];
    for (uint32_t i = 0; i < settings.pass_count; ++i) order[i] = static_cast<int>(i);
    if (g_pending_move == PendingMove::Left && idx > 0) {
      std::swap(order[idx - 1], order[idx]);
    } else if (g_pending_move == PendingMove::Right &&
               idx + 1 < static_cast<int>(settings.pass_count)) {
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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("ReShade DLSSNR addon version. The panel settings are saved to the ReShade configuration.");
  }
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Master switch. When off, no passes run and no DLSSNR GPU work is recorded.");
  }

  SectionDivider();

  // Keep runtime health directly below the master switch, as in the requested
  // layout. This is the single source for live status and stage-ordered passes.
  DrawStatusSection(settings, changed);

  SectionDivider();

  if (PersistentHeader(
          "Preferences", settings.ui_preferences_open, changed,
          "Overlay preferences and the condition used to decide whether DLSSNR should run.")) {
    IndentScope indent;
    changed |= ImGui::Checkbox("Show Tooltips", &settings.show_tooltips);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Show contextual descriptions when hovering controls and section headers.");
    }

    changed |= ImGui::Checkbox("Require DLSS", &settings.only_super_sampling);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Only run when the game is actively using DLSS Super Sampling. Turning this off also allows evaluation on frames without DLSS, which is rarely useful because the model expects a DLSS-path image.");
    }

    if (ImGui::Checkbox("Clone Game Resources", &settings.clone_resources)) {
      DlssGuides::Get().RequestCloneResources(settings.clone_resources);
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Off (default): read the game's current depth/motion resources and the DLSS output directly. On: copy current-frame depth/motion, and the DLSS output when Post-DLSS is enabled, into addon-owned textures. This one option applies to all stages, including Backbuffer; the requested mode is latched safely at a new DLSS evaluation.");
    }
  }

  SectionDivider();

  DrawSavedProfilesSection(settings, changed);

  SectionDivider();

  if (PersistentHeader(
          "Debug", settings.ui_debug_open, changed,
          "Select an internal buffer to replace the final image for diagnosis. The selected pass renders the view.")) {
    IndentScope indent;
    DrawSharedDebugView(settings, changed);
  }

  SectionDivider();

  // The tab bar represents the active pass profiles. The target tag is
  // informational: execution groups Pre, Post, then Backbuffer while keeping
  // the pass order inside each group.
  if (PersistentHeader(
          "Passes", settings.ui_passes_open, changed,
          "Configure each pass profile. Execution order is grouped by target stage, then follows this tab order within each stage.")) {
    IndentScope indent;
    ImGui::TextDisabled("Execution: Pre-DLSS -> Post-DLSS -> Backbuffer; pass order is preserved within each stage.");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Changing a pass target changes its execution group, not its position in the pass list. Status uses the same execution grouping.");
    }

    const bool can_add_pass = settings.pass_count < kMaxPasses;
    WithDisabled(!can_add_pass, [&] {
      if (ImGui::Button("+ Add Pass")) AddPass(settings, changed);
    });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Add a new pass, up to the maximum of %u.", kMaxPasses);
    }
    ImGui::SameLine();
    const bool can_remove_pass = settings.pass_count > 1;
    WithDisabled(!can_remove_pass, [&] {
      if (ImGui::Button("- Remove Pass")) RemoveActivePass(settings, changed);
    });
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Remove the selected pass. At least one pass remains.");
    }

    const StatusSnapshot tab_status = AddonController::Get().GetStatus();
    if (ImGui::BeginTabBar("PassTabs")) {
      for (uint32_t i = 0; i < settings.pass_count; ++i) {
        const PassSettings& pass = settings.passes[i];
        const char* stage_name =
            pass.injection_stage == InjectionStage::PreDLSS ? "Pre-DLSS" :
            pass.injection_stage == InjectionStage::PostDLSS ? "Post-DLSS" :
                                                              "Backbuffer";
        const char* stage_short =
            pass.injection_stage == InjectionStage::PreDLSS ? "Pre" :
            pass.injection_stage == InjectionStage::PostDLSS ? "Post" : "BB";
        const bool pass_failed = tab_status.passes[i].failed;
        const char* state_tag = pass_failed ? "FAIL" :
                                settings.pass_enabled[i] ? "on" : "off";
        const char* state_description = pass_failed ? "failed" :
                                        settings.pass_enabled[i] ? "enabled" :
                                                                   "disabled";
        const char* display_name = pass.custom_name[0] != '\0'
                                       ? pass.custom_name
                                       : nullptr;
        char default_name[24];
        std::snprintf(default_name, sizeof(default_name), "Pass %u", i + 1);
        const char* tab_name = display_name != nullptr ? display_name : default_name;

        char label[128];
        if (display_name != nullptr) {
          std::snprintf(label, sizeof(label), "%s [%s] %s###PassTab%u",
                        display_name, state_tag, stage_short, i);
        } else {
          std::snprintf(label, sizeof(label), "Pass %u [%s] %s###PassTab%u",
                        i + 1, state_tag, stage_short, i);
        }

        const bool force_this = (g_force_select_tab == static_cast<int>(i));
        const ImGuiTabItemFlags tab_flags =
            force_this ? ImGuiTabItemFlags_SetSelected : 0;
        const bool tab_open = ImGui::BeginTabItem(label, nullptr, tab_flags);
        if (ImGui::IsItemClicked()) {
          settings.active_pass = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          Tip("Select Pass %u · %s · target: %s · %s. The pass remains in its original tab position; runtime groups passes by target stage.",
              i + 1, tab_name, stage_name, state_description);
        }
        if (tab_open) {
          ImGui::EndTabItem();
        }
      }
      g_force_select_tab = -1;
      ImGui::EndTabBar();
    }

    if (settings.active_pass >= 0 &&
        settings.active_pass < static_cast<int>(settings.pass_count)) {
      DrawPassSettingsBody(static_cast<uint32_t>(settings.active_pass),
                           settings, changed);
    }
  }

  SectionDivider();

  if (PersistentHeader(
          "Grade Backbuffer", settings.ui_grade_open, changed,
          "Apply an optional final colour grade to the swapchain image after all passes. A debug view replaces the backbuffer and is not graded.")) {
    IndentScope indent;
    DrawGradeSection(
        "grade_backbuffer", "Enabled",
        "Grade the final swapchain image after every pass, immediately before presentation. This is independent of each pass's model-input and model-output grades.",
        settings.grade_backbuffer, changed);
  }

  SectionDivider();

  // HDR applies only when an enabled pass targets Pre-DLSS or Post-DLSS.
  const bool hdr_relevant = AnyPassUsesHdrInjection(settings);
  DrawHdrSection(settings, hdr_relevant, changed);

  SectionDivider();

  // Importance Mask consumes the shared HDR frame snapshot and is unavailable
  // when every enabled pass targets only the Backbuffer.
  DrawImportanceMaskSection(settings, hdr_relevant, changed);

  SectionDivider();

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

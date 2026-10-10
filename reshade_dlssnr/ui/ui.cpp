#include <imgui.h>

#include "ui.h"

#include <cstdio>

#include "gpu/shader_params.h"
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

  // HDR handling applies to both game-resource targets. The section is
  // shared, so it is live whenever any enabled Pre-DLSS or Post-DLSS pass
  // would use it — not just the currently-selected tab.
  const bool hdr_relevant = AnyPassUsesHdrInjection(settings);

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

  // --- HDR section (shared across passes) ---
  DrawHdrSection(settings, hdr_relevant, changed);

  SectionDivider();

  // --- Structure detection (shared across passes; Post-DLSS only) ---
  DrawStructureDetectSection(settings, changed);

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

// Pass bookkeeping: the array plumbing the declarative pass section does not
// own.
//
// Everything the pass body *renders* now comes from the ctl schema (see
// feature/pass/schema.cpp). What is left here is the part
// that is genuinely about the pass array rather than about settings rows: the
// toolbar that permutes and resizes it, the deferred mutation queue those
// buttons feed, and the profile reset. The overlay applies the queue at the
// top of the next frame so pass-order changes cannot invalidate the current
// frame's tab layout.

#include "ui/ui_sections.h"
#include "profiling/profile.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "state/settings.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

// The deferred mutation queue the toolbar writes and the overlay drains.
PendingMove g_pending_move = PendingMove::None;
int g_pending_move_index = -1;
PendingPassAction g_pending_pass_action = PendingPassAction::None;

namespace {

// Reset a pass to its declared defaults, preserving the three things the reset
// button's tooltip promises: the target stage, the custom name, and (via the
// caller) the enabled flag. The three pass-owned sections -- the Pass section
// itself, Grade Model Input, and Grade Model Output -- are reset; the
// preserved values are then written back to the Pass section.
//
// The values that survive are read off the instance, the rest are reset with
// ResetSectionToDefault, and the survivors are written back. The pass's stable
// id -- not its position -- is what identifies the instance throughout.
void ResetPassProfile(feature::pass::PassId id) {
  DLSSNR_PROFILE_UI_L2("ResetPassProfile");
  const ctl::InstanceKey key = feature::pass::PassKey(id);
  const std::string section = feature::pass::kSection;
  ctl::ValueStore& store = feature::SettingsStore();

  const int target = store.Read(section, key, feature::pass::kTarget).as_int;
  const bool pass_enabled =
      store.Read(section, key, feature::pass::kPassEnabled).as_bool;
  const std::string name =
      store.Read(section, key, feature::pass::kCustomName).as_string;

  store.ResetSectionToDefault(section, key);
  store.ResetSectionToDefault(feature::pass::kGradeInputSection, key);
  store.ResetSectionToDefault(feature::pass::kGradeOutputSection, key);

  store.Write(section, key, feature::pass::kTarget, ctl::Value::Enum(target));
  store.Write(section, key, feature::pass::kPassEnabled,
              ctl::Value::Bool(pass_enabled));
  store.Write(section, key, feature::pass::kCustomName,
              ctl::Value::String(name));
}

}  // namespace

// Draws the five pass-array controls: move left, move right, reset, add,
// remove. All of them act on tab POSITIONS, and every mutation is deferred to
// the next frame (see the globals in the header) because the toolbar is drawn
// inside the tab-bar loop.
//
// Positions are view-only. Nothing here touches a store value: a reorder
// permutes Settings::pass_order, and the store's instances do not move.
void DrawPassToolbar(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawPassToolbar");
  const int pass_total = static_cast<int>(settings.pass_order.size());
  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);

  if (active_it != settings.pass_order.end()) {
    const ctl::InstanceKey active_key = feature::pass::PassKey(*active_it);
    ctl::ValueStore& store = feature::SettingsStore();
    bool enabled = store.Read(feature::pass::kSection, active_key,
                              feature::pass::kPassEnabled).as_bool;
    if (ImGui::Checkbox("Enable##pass_toolbar_enabled", &enabled)) {
      store.Write(feature::pass::kSection, active_key,
                  feature::pass::kPassEnabled, ctl::Value::Bool(enabled));
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Enable or disable the selected pass without opening its settings.");
    }
    ImGui::SameLine();
  }
  const int position =
      active_it == settings.pass_order.end()
          ? 0
          : static_cast<int>(std::distance(settings.pass_order.begin(), active_it));

    // Stable ids via "##": the visible glyph stays fixed while the label text
  // (which never changes here) cannot disturb widget identity.
  const bool can_move_left = position > 0;
  WithDisabled(!can_move_left, [&] {
    if (ImGui::Button("<##pass_move_left", ImVec2(kResetButtonWidth, 0)) &&
        can_move_left) {
      g_pending_move = PendingMove::Left;
      g_pending_move_index = position;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move pass %d one position earlier. Execution still groups by target stage.",
        position + 1);
  }

  ImGui::SameLine();
  const bool can_move_right = position + 1 < pass_total;
  WithDisabled(!can_move_right, [&] {
    if (ImGui::Button(">##pass_move_right", ImVec2(kResetButtonWidth, 0)) &&
        can_move_right) {
      g_pending_move = PendingMove::Right;
      g_pending_move_index = position;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Move pass %d one position later. Execution still groups by target stage.",
        position + 1);
  }

  ImGui::SameLine();
  if (ImGui::Button("R##pass_reset", ImVec2(kResetButtonWidth, 0))) {
    ResetPassProfile(settings.pass_order[position]);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset pass %d's model, residual, grade, sharpening, and shader settings to defaults. Its name, target, and enabled state are preserved.",
        position + 1);
  }

  ImGui::SameLine();
  const bool can_add_pass =
      static_cast<int>(settings.pass_order.size()) < kMaxPasses;
  WithDisabled(!can_add_pass, [&] {
    if (ImGui::Button("+##pass_add", ImVec2(kResetButtonWidth, 0)) &&
        can_add_pass) {
      // Defer the array mutation to the next frame; the tab bar is mid-layout.
      g_pending_pass_action = PendingPassAction::Add;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Add a new pass after pass %d, up to the maximum of %u.", position + 1,
        kMaxPasses);
  }

  ImGui::SameLine();
  const bool can_remove_pass = settings.pass_order.size() > 1;
  WithDisabled(!can_remove_pass, [&] {
    if (ImGui::Button("x##pass_remove", ImVec2(kResetButtonWidth, 0)) &&
        can_remove_pass) {
      g_pending_pass_action = PendingPassAction::Remove;
    }
  });
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Remove pass %d. At least one pass remains.", position + 1);
  }
}

}  // namespace reshade_dlssnr

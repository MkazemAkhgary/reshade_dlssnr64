#include <imgui.h>

#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include "ctl/draw.h"
#include "ctl/usage/ui_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/grade/settings.h"
#include "feature/grade/view.h"
#include "feature/hdr/settings.h"
#include "feature/importance_mask/settings.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/pass/ui.h"
#include "feature/preferences/settings.h"
#include "hooks/dlss_guides.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"
#include "version.h"

namespace reshade_dlssnr {

// Set by ApplyPassReorder and consumed by DrawOverlay on the next frame. Tab
// IDs are positional, so after a reorder ImGui still has the tab at the old
// position selected and would overwrite active_pass_id back to it. This forces
// the tab that now holds the active pass to be selected instead.
int g_force_select_tab = -1;

// The overlay's per-frame scratch state. It is handed to the framework as an
// opaque pointer so the open-state callbacks can share one context.
namespace {

// The caller's opaque handle. The open-state callbacks below ignore it and
// read the global settings directly, but DrawOptions still threads it through,
// so it needs to be something.
void* Ui() {
  static int token = 0;
  return &token;
}

// ---------------------------------------------------------------------------
// Section open/closed state. The framework owns no UI state, so these two
// callbacks translate between a section name and the matching Settings flag.
// ---------------------------------------------------------------------------

bool* SectionOpenFlag(Settings& settings, const std::string& name) {
  if (name == feature::preferences::kSection) return &settings.ui_preferences_open;
  if (name == feature::debug_log::kSection) return &settings.ui_debug_open;
  if (name == feature::grade::kSection) return &settings.ui_grade_open;
  if (name == feature::pass::kGradeInputSection) return &settings.ui_grade_input_open;
  if (name == feature::pass::kGradeOutputSection) return &settings.ui_grade_output_open;
  if (name == feature::hdr::kSection) return &settings.ui_hdr_open;
  if (name == feature::pass::kSection) return &settings.ui_passes_open;
  if (name == feature::importance_mask::kSection) {
    return &settings.ui_importance_mask_open;
  }
  return nullptr;
}

bool ReadSectionOpen(void* /*context*/, const std::string& name) {
  Settings& settings = GetSettings();
  bool* flag = SectionOpenFlag(settings, name);
  return flag != nullptr ? *flag : true;
}

void WriteSectionOpen(void* /*context*/, const std::string& name, bool open) {
  Settings& settings = GetSettings();
  if (bool* flag = SectionOpenFlag(settings, name)) *flag = open;
}

// The HDR exposure tabs are drawn by their hook rather than as categories.
// The pass section uses these callbacks for its nested Model / Strength /
// Advanced / Soft Compression / Residual Edits groups.
bool* CategoryOpenFlag(Settings& settings, const std::string& section,
                       const std::string& category) {
  if (section != feature::pass::kSection) return nullptr;
  if (category == "Model") return &settings.ui_model_open;
  if (category == "Strength") return &settings.ui_strength_open;
  if (category == "Advanced/Soft Compression") return &settings.ui_soft_compression_open;
  if (category == "Advanced") return &settings.ui_pass_advanced_open;
  if (category == "Advanced/Residual Edits") return &settings.ui_residual_open;
  return nullptr;
}

bool ReadCategoryOpen(void* /*context*/, const std::string& section,
                      const std::string& category) {
  Settings& settings = GetSettings();
  bool* flag = CategoryOpenFlag(settings, section, category);
  return flag != nullptr ? *flag : true;
}

void WriteCategoryOpen(void* /*context*/, const std::string& section,
                       const std::string& category, bool open) {
  Settings& settings = GetSettings();
  if (bool* flag = CategoryOpenFlag(settings, section, category)) *flag = open;
}

ctl::DrawOptions MakeOptions(bool headers, bool indent) {
  ctl::DrawOptions options;
  options.section_open = &ReadSectionOpen;
  options.set_section_open = &WriteSectionOpen;
  options.category_open = &ReadCategoryOpen;
  options.set_category_open = &WriteCategoryOpen;
  options.category_style = ctl::DrawOptions::CategoryStyle::SubHeader;
  options.ui_context = Ui();
  options.draw_headers = headers;
  options.indent_bodies = indent;
  return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pass-order mutation. Stable pass IDs remain attached to their stored values;
// an order change permutes the vector and nothing else.
// ---------------------------------------------------------------------------

// Reset the backbuffer grade to its declared defaults. Writes through the
// store rather than into a mirror struct: the store is the only owner of the
// value, and the header's local copy is refreshed from it on the next frame.
void ResetBackbufferGrade(void* /*context*/) {
  const ctl::InstanceKey key = ctl::InstanceKey::FromInt(0);
  ctl::Store().ResetSectionToDefault(feature::grade::kSection, key);
}

// Reorder the pass_order vector. `order` holds the source position of each
// destination position. Nothing in the store moves: each pass's values are
// keyed by its stable id.
// True when the pass at `position` is enabled, read from the store. The tab
// bar and the body both need it, and both key it by the pass's stable id.

void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  const size_t total = settings.pass_order.size();
  feature::pass::PassOrder reordered(total);
  for (size_t i = 0; i < total; ++i) {
    const int source = order[i];
    if (source < 0 || static_cast<size_t>(source) >= total) continue;  // defensive
    reordered[i] = settings.pass_order[source];
  }
  settings.pass_order.swap(reordered);

  // active_pass_id is a stable id, so a reorder does not need to update it.
  // Fall back to the first pass only if the selected id somehow vanished.
  if (std::find(settings.pass_order.begin(), settings.pass_order.end(),
                settings.active_pass_id) == settings.pass_order.end()) {
    settings.active_pass_id =
        settings.pass_order.empty() ? 0 : settings.pass_order.front();
  }

  // Keep the runtime's session map in step with the new order.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// Insert a new pass after the selected one. The inserted pass gets a fresh
// stable id, so its settings start empty and every other pass keeps its own.
void AddPass(Settings& settings, bool& changed) {
  const size_t total = settings.pass_order.size();
  if (total >= kMaxPasses) return;
  const size_t source = static_cast<size_t>(
      std::clamp(static_cast<int>(settings.active_pass_id), 0, static_cast<int>(total) - 1));
  const size_t insert_at = source + 1;

  feature::pass::PassOrder order = settings.pass_order;
  const feature::pass::PassId new_id = NewPassId();
  order.insert(order.begin() + static_cast<ptrdiff_t>(insert_at), new_id);
  settings.pass_order.swap(order);
  settings.active_pass_id = new_id;

  // Create the new pass's session so it is ready the first frame it runs.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// Remove the selected pass. Its id leaves the order and is never reused, so
// its settings stay in the store unread rather than being handed to a pass
// created later.
void RemoveActivePass(Settings& settings, bool& changed) {
  if (settings.pass_order.size() <= 1) return;
  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  if (active_it == settings.pass_order.end()) return;
  const size_t remove_at =
      static_cast<size_t>(std::distance(settings.pass_order.begin(), active_it));

  // The id has to be captured before the erase: once it leaves pass_order it
  // is not reachable from anywhere else, and its keys are no longer rewritten
  // by Save, so they must be erased from the config explicitly.
  const feature::pass::PassId removed_id = *active_it;

  feature::pass::PassOrder order = settings.pass_order;
  order.erase(order.begin() + static_cast<ptrdiff_t>(remove_at));
  settings.pass_order.swap(order);

  if (settings.pass_order.empty()) {
    settings.active_pass_id = 0;
  } else {
    const size_t next_index =
        std::min(remove_at, settings.pass_order.size() - 1);
    settings.active_pass_id = settings.pass_order[next_index];
  }

  // Erase the removed pass's ctl keys so the retired id leaves no orphan
  // keys behind in the config.
  ErasePassKeys(removed_id);

  // EnsureSessions drops the removed pass's session and its NGX feature.
  AddonController::Get().EnsureSessions(settings.pass_order);
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
  enum class PendingProfileAction { None, Delete, Save, Load };
  static PendingProfileAction pending_action = PendingProfileAction::None;
  static ProfileId pending_profile_id = 0;
  static bool profile_name_error = false;
  static ProfileId renaming_profile_id = 0;
  static char rename_profile_name[kSavedProfileNameCapacity] = {};
  static bool rename_profile_error = false;
  static int profile_feedback_index = -1;
  static char profile_feedback[160] = {};
  static char new_profile_name[kSavedProfileNameCapacity] = {};

  auto clear_pending = []() {
    pending_action = PendingProfileAction::None;
    pending_profile_id = 0;
  };

  if (profile_count == 0) {
    ImGui::TextDisabled("No saved presets.");
  }

  for (uint32_t i = 0; i < profile_count && i < kMaxSavedProfiles; ++i) {
    bool deleted_this_frame = false;
    ImGui::PushID(static_cast<int>(i));
    const bool is_renaming = renaming_profile_id == profiles[i].id;
    ImGui::AlignTextToFramePadding();
    if (is_renaming) {
      ImGui::SetNextItemWidth(210.0f);
      ImGui::InputText("##rename_profile_name", rename_profile_name,
                       sizeof(rename_profile_name));
      ImGui::SameLine();
      if (ImGui::Button("Save Name")) {
        if (RenameSavedProfile(profiles[i].id, rename_profile_name)) {
          renaming_profile_id = 0;
          rename_profile_error = false;
          profile_feedback_index = static_cast<int>(i);
          std::snprintf(profile_feedback, sizeof(profile_feedback),
                        "Renamed preset to: %s", rename_profile_name);
          changed = true;
        } else {
          rename_profile_error = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel Rename")) {
        renaming_profile_id = 0;
        rename_profile_error = false;
      }
    } else {
      ImGui::TextUnformatted(profiles[i].name[0] != '\0'
                                 ? profiles[i].name
                                 : "(unnamed preset)");
      ImGui::SameLine();
      if (ImGui::Button("Rename")) {
        renaming_profile_id = profiles[i].id;
        std::snprintf(rename_profile_name, sizeof(rename_profile_name), "%s",
                      profiles[i].name);
        rename_profile_error = false;
      }
      ImGui::SameLine();
    }
    if (ImGui::Button("Load")) {
      pending_action = PendingProfileAction::Load;
      pending_profile_id = profiles[i].id;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Load this preset into the active settings and save it as the current ReShade.ini configuration.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
      pending_action = PendingProfileAction::Save;
      pending_profile_id = profiles[i].id;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Overwrite this saved preset with the current settings.");
    }
    ImGui::SameLine();
    if (ImGui::Button("x")) {
      if (pending_action == PendingProfileAction::Delete &&
          pending_profile_id == profiles[i].id) {
        clear_pending();
      } else {
        pending_action = PendingProfileAction::Delete;
        pending_profile_id = profiles[i].id;
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Remove this preset after confirming.");
    }
    ImGui::PopID();
    if (renaming_profile_id == profiles[i].id && rename_profile_error) {
      ImGui::TextDisabled("Preset name is blank, too long, or already in use.");
    }

    if (pending_profile_id == profiles[i].id &&
        pending_action != PendingProfileAction::None) {
      const char* prompt =
          pending_action == PendingProfileAction::Delete
              ? "Remove this preset?"
          : pending_action == PendingProfileAction::Save
              ? "Overwrite this preset?"
              : "Load this preset?";
      ImGui::Indent();
      ImGui::TextUnformatted(prompt);
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.12f, 0.12f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.16f, 0.16f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.08f, 0.08f, 1.0f));
      if (ImGui::Button("Yes")) {
        if (pending_action == PendingProfileAction::Delete) {
          DeleteSavedProfile(profiles[i].id);
          deleted_this_frame = true;
        } else if (pending_action == PendingProfileAction::Save) {
          if (SaveCurrentToProfile(profiles[i].id)) {
            profile_feedback_index = static_cast<int>(i);
            std::snprintf(profile_feedback, sizeof(profile_feedback),
                          "Saved current settings to: %s", profiles[i].name);
          }
        } else {
          if (LoadSavedProfile(profiles[i].id)) {
            changed = true;
            profile_feedback_index = static_cast<int>(i);
            std::snprintf(profile_feedback, sizeof(profile_feedback),
                          "Loaded preset: %s", profiles[i].name);
          }
        }
        clear_pending();
      }
      ImGui::PopStyleColor(3);
      if (!deleted_this_frame) {
        ImGui::SameLine();
        if (ImGui::Button("No")) clear_pending();
      }
      ImGui::Unindent();
    }
    if (profile_feedback_index == static_cast<int>(i)) {
      ImGui::TextDisabled("%s", profile_feedback);
    }
    if (deleted_this_frame) {
      profile_feedback_index = -1;
      profile_feedback[0] = '\0';
      break;
    }
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
    profile_feedback_index = -1;
    profile_feedback[0] = '\0';
  }
  ImGui::SameLine();
  if (ImGui::Button("Save as New Preset") && new_profile_name[0] != '\0') {
    if (SaveCurrentAsProfile(new_profile_name)) {
      std::snprintf(profile_feedback, sizeof(profile_feedback),
                    "Saved new preset: %s", new_profile_name);
      profile_feedback_index = -2;
      new_profile_name[0] = '\0';
      clear_pending();
      profile_name_error = false;
    } else {
      profile_feedback_index = -1;
      profile_feedback[0] = '\0';
      profile_name_error = true;
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Save all current DLSSNR settings under a new name. Up to %u presets can be stored in ReShade.ini.", kMaxSavedProfiles);
  }
  if (profile_feedback_index == -2) ImGui::TextDisabled("%s", profile_feedback);
  if (profile_name_error) {
    ImGui::TextDisabled("Preset name is blank, too long, or already in use.");
  }
  if (profile_count >= kMaxSavedProfiles) ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// The pass section's body. The section header and the tab row are drawn here
// rather than by the framework, because the tab bar needs a reserved child
// column so tabs can never render or be hit-tested under the toolbar.
// ---------------------------------------------------------------------------

namespace {

void DrawTabsRowAndBody(ctl::ValueStore& store, Settings& settings,
                        bool& changed) {
  // The section header keeps the legacy label.
  if (!PersistentHeader(
          "Passes", settings.ui_passes_open, changed,
          "Configure each pass profile. Execution order is grouped by target stage, then follows this tab order within each stage.")) {
    return;
  }

  IndentScope indent;
  ImGui::TextDisabled("Execution: Pre-DLSS -> Post-DLSS -> Backbuffer; pass order is preserved within each stage.");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Changing a pass target changes its execution group, not its position in the pass list. Status uses the same execution grouping.");
  }

  const StatusSnapshot tab_status = AddonController::Get().GetStatus();

  // Reproduce the tab-bar-in-a-child-column idiom: the bar spans only its own
  // column, so no tab can be drawn or clicked under the toolbar buttons.
  const float toolbar_width = PassToolbarWidth();
  const float row_width = ImGui::GetContentRegionAvail().x;
  const float tabs_column_width = std::max(120.0f, row_width - toolbar_width);
  const ImVec2 tabs_row_pos = ImGui::GetCursorScreenPos();
  const float tabs_row_height =
      ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::BeginChild("##pass_tabs_column",
                        ImVec2(tabs_column_width, tabs_row_height), false,
                        ImGuiWindowFlags_NoScrollbar |
                            ImGuiWindowFlags_NoScrollWithMouse)) {
    // Custom tab row: styled buttons that behave like tabs. ImGui's native
    // tab bar caches tabs by ID and keeps them in first-submitted order, so
    // reordering them requires changing the tab bar ID -- which renders the
    // whole bar empty for a frame and flickers. Buttons have no cross-frame
    // caching, so a reorder is instantaneous.
    {
      const ImVec4 active_color =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      const ImVec4 hovered_color =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
      const ImVec4 inactive_color =
          ImGui::GetStyleColorVec4(ImGuiCol_Button);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

      bool first_tab = true;
      for (uint32_t position = 0; position < settings.pass_order.size();
           ++position) {
        // Every per-pass fact comes from the store, keyed by the pass's
        // stable id. Tab position is a view over the order vector, never an
        // identity.
        const feature::pass::PassId id = settings.pass_order[position];
        const feature::pass::PassSnapshot pass =
            feature::pass::Read(store, feature::pass::PassKey(id));
        const char* stage_name =
            pass.target == 0 ? "Pre-DLSS" :
            pass.target == 1 ? "Post-DLSS" : "Backbuffer";
        const char* stage_short =
            pass.target == 0 ? "Pre" :
            pass.target == 1 ? "Post" : "BB";
        // Status rows are positional (see RefreshStatus), not id-indexed.
        const bool pass_failed = tab_status.passes[position].failed;
        const char* state_tag = pass_failed ? "FAIL" :
                                pass.enabled ? "on" : "off";
        const char* state_description = pass_failed ? "failed" :
                                        pass.enabled ? "enabled" :
                                                       "disabled";
        const char* display_name =
            pass.custom_name.empty() ? nullptr : pass.custom_name.c_str();
        char default_name[24];
        std::snprintf(default_name, sizeof(default_name), "Pass %u",
                      position + 1);

        char label[128];
        if (display_name != nullptr) {
          std::snprintf(label, sizeof(label), "%s [%s] %s##PassTab%u",
                        display_name, state_tag, stage_short, id);
        } else {
          std::snprintf(label, sizeof(label), "Pass %u [%s] %s##PassTab%u",
                        position + 1, state_tag, stage_short, id);
        }

        if (!first_tab) ImGui::SameLine();
        first_tab = false;

        const bool is_active = (settings.active_pass_id == id);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              is_active ? active_color : inactive_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered_color);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_color);

        if (ImGui::Button(label)) {
          settings.active_pass_id = id;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          Tip("Select Pass %u - %s - target: %s - %s. The pass remains in its original tab position; runtime groups passes by target stage.",
              position + 1, default_name, stage_name, state_description);
        }
        ImGui::PopStyleColor(3);
      }

      ImGui::PopStyleVar();
      // The variable is a leftover of the ImGui tab-bar approach; the button
      // row selects by writing settings.active_pass_id directly.
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  ImGui::SetCursorScreenPos(ImVec2(tabs_row_pos.x + tabs_column_width +
                                       ImGui::GetStyle().ItemSpacing.x,
                                   tabs_row_pos.y));
  DrawPassToolbar(settings, changed);

  ImGui::SetCursorScreenPos(
      ImVec2(tabs_row_pos.x, tabs_row_pos.y + tabs_row_height));

  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  if (active_it == settings.pass_order.end()) return;

  // Keep the pass's own enable checkbox and name editable while it is off.
  const feature::pass::PassId active_id = *active_it;
  ImGui::PushID(static_cast<int>(active_id));
  const ctl::InstanceKey key = feature::pass::PassKey(active_id);
  bool local_changed = false;
  // Keep the body interactive while the pass is off, including its own Enable
  // checkbox. Runtime execution alone observes the pass-enabled value.
  ctl::DrawSection(store, feature::pass::kSection, key, local_changed,
                   MakeOptions(/*headers=*/false, /*indent=*/false));
  changed |= local_changed;
  ImGui::PopID();
}

bool FeatureRowGate(void*, ctl::ValueStore& store, const std::string& section,
                    const ctl::InstanceKey& key) {
  if (section != feature::importance_mask::kSection) return true;
  const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
  return store.Read(feature::hdr::kSection, shared, feature::hdr::kEnable).as_bool;
}

void DrawSectionHeaderAction(void*, const std::string& section,
                             bool& changed) {
  if (section != feature::grade::kSection) return;
  const float right = ImGui::GetCursorPosX() +
                      ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(std::max(0.0f, right - kResetButtonWidth));
  if (ImGui::Button("R##backbuffer_grade_reset",
                    ImVec2(kResetButtonWidth, 0.0f))) {
    ResetBackbufferGrade(nullptr);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset all Grade Backbuffer settings to their defaults.");
  }
}

// Draw one single-instance section with the framework, keeping the legacy
// header. The identity function keeps the call site a one-liner.
void DrawOne(ctl::ValueStore& store, const char* name, bool& changed,
             ctl::DrawOptions::HeaderAction header_action = nullptr) {
  bool local_changed = false;
  ctl::DrawOptions options = MakeOptions(/*headers=*/true, /*indent=*/true);
  options.header_action = header_action;
  if (std::string(name) == feature::importance_mask::kSection) {
    options.row_gate = &FeatureRowGate;
  }
  ctl::DrawSection(store, name, ctl::InstanceKey::FromInt(0), local_changed,
                   options);
  changed |= local_changed;
}


}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  bool changed = false;

  ctl::ValueStore& store = ctl::Store();
  const ctl::InstanceKey shared_key = ctl::InstanceKey::FromInt(0);
  g_show_tooltips = store.Read(feature::preferences::kSection, shared_key,
                               feature::preferences::kShowTooltips).as_bool;

  // Pass-order mutations are deferred until outside the tab-bar iteration.
  // This keeps ImGui's positional tab IDs and the pass order in sync.
  if (g_pending_pass_action != PendingPassAction::None) {
    const PendingPassAction action = g_pending_pass_action;
    g_pending_pass_action = PendingPassAction::None;
    g_pending_move = PendingMove::None;
    g_pending_move_index = -1;
    if (action == PendingPassAction::Add) {
      AddPass(settings, changed);
    } else if (action == PendingPassAction::Remove) {
      RemoveActivePass(settings, changed);
    }
    SaveSettings();
    changed = false;
  }

  if (g_pending_move != PendingMove::None && g_pending_move_index >= 0 &&
      g_pending_move_index <
          static_cast<int>(settings.pass_order.size())) {
    const int idx = g_pending_move_index;
    int order[kMaxPasses];
    const uint32_t total = static_cast<uint32_t>(settings.pass_order.size());
    for (uint32_t i = 0; i < total; ++i) order[i] = static_cast<int>(i);
    if (g_pending_move == PendingMove::Left && idx > 0) {
      std::swap(order[idx - 1], order[idx]);
    } else if (g_pending_move == PendingMove::Right &&
               idx + 1 < static_cast<int>(total)) {
      std::swap(order[idx], order[idx + 1]);
    }
    g_pending_move = PendingMove::None;
    ApplyPassReorder(settings, order, changed);
    SaveSettings();
    changed = false;
  }


  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("ReShade DLSSNR addon version. The panel settings are saved to the ReShade configuration.");
  }
  ImGui::Separator();

  // Master enable is a hand-drawn row: it is not part of any section.
  if (ImGui::Checkbox("Enable DLSSNR", &settings.enabled)) {
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Master switch. When off, no passes run and no DLSSNR GPU work is recorded.");
  }

  SectionDivider();

  // Status is a readout, not settings, so it stays a hook-driven section with
  // its own header and the existing DrawStatusSection.
  DrawStatusSection(settings, changed);

  SectionDivider();

  DrawOne(store, feature::preferences::kSection, changed);
  // The preference row can change the store during this draw; update the
  // tooltip gate before drawing any later controls in the same frame.
  g_show_tooltips = store.Read(feature::preferences::kSection, shared_key,
                               feature::preferences::kShowTooltips).as_bool;

  SectionDivider();

  DrawSavedProfilesSection(settings, changed);

  SectionDivider();

  DrawOne(store, feature::debug_log::kSection, changed);

  SectionDivider();

  DrawTabsRowAndBody(store, settings, changed);

  SectionDivider();

  // Grade Backbuffer uses ctl's native section-header toggle. The reset action
  // is an application-owned header action; the Enabled row is not duplicated.
  DrawOne(store, feature::grade::kSection, changed,
          &DrawSectionHeaderAction);

  SectionDivider();

  // HDR: the framework draws the header and the rows. Inside, the exposure tab
  // bar and selected layer are drawn by the ExposureTabs hook, declared between
  // the headroom row and the BT.2446 curve rows in the HDR schema.
  DrawOne(store, feature::hdr::kSection, changed);

  SectionDivider();

  // Importance Mask is only usable when HDR is on: without HDR there is no
  // measurement to filter. Keep the mask's header toggle interactive, while
  // the host applies the cross-feature gate to the tuning rows.
  DrawOne(store, feature::importance_mask::kSection, changed);

  SectionDivider();

  // This is intentionally refreshed from the store every overlay frame. The
  // guide layer latches the request at its next NGX evaluation, so a checkbox
  // edit is visible without reloading the addon.
  DlssGuides::Get().RequestCloneResources(
      store.Read(feature::preferences::kSection, shared_key,
                 feature::preferences::kCloneResources).as_bool);

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

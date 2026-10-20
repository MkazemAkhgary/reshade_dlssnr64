#include <imgui.h>

#include "ui.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ctl/draw.h"
#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/grade/settings.h"
#include "feature/grade/view.h"
#include "feature/hdr/settings.h"
#include "feature/importance_mask/settings.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/preferences/settings.h"
#include "feature/ui_state/settings.h"
#include "feature/ui_state/view.h"
#include "hooks/dlss_guides.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"
#include "version.h"

namespace reshade_dlssnr {

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
// Section open/closed state is stored as ordinary global ctl settings.
// ---------------------------------------------------------------------------

const char* SectionOpenSetting(const std::string& name) {
  if (name == feature::preferences::kSection) return feature::ui_state::kPreferencesOpen;
  if (name == feature::debug_log::kSection) return feature::ui_state::kDebugOpen;
  if (name == feature::grade::kSection) return feature::ui_state::kGradeOpen;
  if (name == feature::pass::kGradeInputSection) return feature::ui_state::kGradeInputOpen;
  if (name == feature::pass::kGradeOutputSection) return feature::ui_state::kGradeOutputOpen;
  if (name == feature::hdr::kSection) return feature::ui_state::kHdrOpen;
  if (name == feature::hdr::kGameSection) return feature::ui_state::kHdrGameOpen;
  if (name == feature::hdr::kAdaptiveSection) return feature::ui_state::kHdrAdaptiveOpen;
  if (name == feature::hdr::kLocalSection) return feature::ui_state::kHdrLocalOpen;
  if (name == feature::pass::kSection) return feature::ui_state::kPassesOpen;
  if (name == feature::importance_mask::kSection) return feature::ui_state::kImportanceMaskOpen;
  return nullptr;
}

bool ReadSectionOpen(void* /*context*/, const std::string& name) {
  const char* setting = SectionOpenSetting(name);
  return setting != nullptr ? feature::ui_state::Read(setting) : true;
}

void WriteSectionOpen(void* /*context*/, const std::string& name, bool open) {
  const char* setting = SectionOpenSetting(name);
  if (setting != nullptr) feature::ui_state::Write(setting, open);
}

const char* CategoryOpenSetting(const std::string& section,
                                const std::string& category) {
  if (section == feature::hdr::kSection) {
    if (category == "Advanced") return feature::ui_state::kHdrAdvancedOpen;
    if (category == "Diagnostics") return feature::ui_state::kHdrDiagnosticsOpen;
    return nullptr;
  }
  if (section == feature::hdr::kLocalSection &&
      category == "Advanced") {
    return feature::ui_state::kHdrLocalAdvancedOpen;
  }
  if (section == feature::hdr::kAdaptiveSection &&
      category == "Filters") {
    return feature::ui_state::kHdrAdaptiveFiltersOpen;
  }
  if (section != feature::pass::kSection) return nullptr;
  if (category == "Model") return feature::ui_state::kModelOpen;
  if (category == "Strength") return feature::ui_state::kStrengthOpen;
  if (category == "Temporal Reprojection") return feature::ui_state::kTemporalOpen;
  if (category == "Soft Compression") return feature::ui_state::kSoftCompressionOpen;
  if (category == "Residual Edits") return feature::ui_state::kResidualOpen;
  return nullptr;
}

bool ReadCategoryOpen(void* /*context*/, const std::string& section,
                      const std::string& category) {
  const char* setting = CategoryOpenSetting(section, category);
  return setting != nullptr ? feature::ui_state::Read(setting) : true;
}

void WriteCategoryOpen(void* /*context*/, const std::string& section,
                       const std::string& category, bool open) {
  const char* setting = CategoryOpenSetting(section, category);
  if (setting != nullptr) feature::ui_state::Write(setting, open);
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
  feature::SettingsStore().ResetSectionToDefault(feature::grade::kSection, key);
}

// Reorder the pass_order vector. `order` holds the source position of each
// destination position. Nothing in the store moves: each pass's values are
// keyed by its stable id.
// True when the pass at `position` is enabled, read from the store. The tab
// bar and the body both need it, and both key it by the pass's stable id.

void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  DLSSNR_PROFILE_UI_L2("ApplyPassReorder");
  const size_t total = settings.pass_order.size();
  feature::pass::PassOrder reordered(total);
  for (size_t i = 0; i < total; ++i) {
    const int source = order[i];
    if (source < 0 || static_cast<size_t>(source) >= total) continue;  // defensive
    reordered[i] = settings.pass_order[source];
  }
  SetPassOrder(settings, std::move(reordered));

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
  DLSSNR_PROFILE_UI_L2("AddPass");
  const size_t total = settings.pass_order.size();
  if (total >= kMaxPasses) return;
  // active_pass_id is an opaque id, not an index; resolve its position.
  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  const size_t insert_at =
      active_it == settings.pass_order.end()
          ? total
          : static_cast<size_t>(std::distance(settings.pass_order.begin(),
                                              active_it)) + 1;

  feature::pass::PassOrder order = settings.pass_order;
  const feature::pass::PassId new_id = NewPassId();
  order.insert(order.begin() + static_cast<ptrdiff_t>(insert_at), new_id);
  SetPassOrder(settings, std::move(order));
  settings.active_pass_id = new_id;

  // Create the new pass's session so it is ready the first frame it runs.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// Remove the selected pass. Its id leaves the order and is never reused, so
// its settings stay in the store unread rather than being handed to a pass
// created later.
void RemoveActivePass(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("RemoveActivePass");
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
  SetPassOrder(settings, std::move(order));

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

  // EnsureSessions removes it from the live map and defers releasing its NGX
  // feature/resources until work recorded by earlier frames has completed.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

void DrawSavedProfilesSection(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawSavedProfilesSection");
  if (!PersistentSettingHeader(
          "Presets", feature::ui_state::kPresetsOpen, changed,
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
  static bool rename_focus_pending = false;
  static int profile_feedback_index = -1;
  static char profile_feedback[160] = {};
  static char new_profile_name[kSavedProfileNameCapacity] = {};

  auto clear_pending = []() {
    pending_action = PendingProfileAction::None;
    pending_profile_id = 0;
  };

  if (profile_count == 0) {
    ImGui::TextDisabled("No saved presets.");
  } else {
    // Fixed-width action column so a rename row (Save Name / Cancel) and a
    // normal row (Rename / Load / Save / x) share the same column width and
    // do not make the table jitter when switching modes. Computed from the
    // wider of the two button sets.
    const float frame_pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_normal =
        ImGui::CalcTextSize("Rename").x + ImGui::CalcTextSize("Load").x +
        ImGui::CalcTextSize("Save").x + ImGui::CalcTextSize("x").x +
        4.0f * frame_pad + 3.0f * spacing;
    const float row_rename =
        ImGui::CalcTextSize("Save Name").x + ImGui::CalcTextSize("Cancel").x +
        2.0f * frame_pad + spacing;
    const float action_width =
        row_normal > row_rename ? row_normal : row_rename;

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##saved_profiles", 2, table_flags)) {
      ImGui::TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,
                              action_width);

      for (uint32_t i = 0; i < profile_count && i < kMaxSavedProfiles; ++i) {
        bool deleted_this_frame = false;
        ImGui::PushID(static_cast<int>(i));
        const bool is_renaming = renaming_profile_id == profiles[i].id;

        ImGui::TableNextRow();

        // Name column: the preset name, or the rename input while editing.
        ImGui::TableSetColumnIndex(0);
        bool rename_commit = false;
        bool rename_cancel = false;
        if (is_renaming) {
          if (rename_focus_pending) {
            ImGui::SetKeyboardFocusHere();
            rename_focus_pending = false;
          }
          ImGui::SetNextItemWidth(-FLT_MIN);
          const bool enter_pressed = ImGui::InputText(
              "##rename_profile_name", rename_profile_name,
              sizeof(rename_profile_name),
              ImGuiInputTextFlags_EnterReturnsTrue |
                  ImGuiInputTextFlags_AutoSelectAll);
          if (ImGui::IsItemHovered()) {
            Tip("Enter saves the name; Escape cancels editing.");
          }
          if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            rename_cancel = true;
          } else if (enter_pressed) {
            rename_commit = true;
          }
        } else {
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(profiles[i].name[0] != '\0'
                                     ? profiles[i].name
                                     : "(unnamed preset)");
        }

        // Actions column. While renaming, only Save Name and Cancel are
        // shown: Load / Save / x would act on a preset whose name is still
        // being edited, and hiding them gives the row a single meaning.
        ImGui::TableSetColumnIndex(1);
        if (is_renaming) {
          if (ImGui::Button("Save Name")) rename_commit = true;
          ImGui::SameLine();
          if (ImGui::Button("Cancel")) rename_cancel = true;
        } else {
          if (ImGui::Button("Rename")) {
            renaming_profile_id = profiles[i].id;
            std::snprintf(rename_profile_name, sizeof(rename_profile_name),
                          "%s", profiles[i].name);
            rename_profile_error = false;
            rename_focus_pending = true;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip("Edit this preset's name. Enter saves; Escape cancels.");
          }
          ImGui::SameLine();
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
        }

        // Rename actions apply after the row has been drawn, so the text the
        // user typed is committed (or discarded) exactly once per frame.
        if (rename_commit) {
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
        } else if (rename_cancel) {
          renaming_profile_id = 0;
          rename_profile_error = false;
          rename_focus_pending = false;
        }

        ImGui::PopID();

        if (renaming_profile_id == profiles[i].id && rename_profile_error) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextDisabled(
              "Preset name is blank, too long, or already in use.");
        }

        if (pending_profile_id == profiles[i].id &&
            pending_action != PendingProfileAction::None) {
          const char* prompt =
              pending_action == PendingProfileAction::Delete
                  ? "Remove this preset?"
              : pending_action == PendingProfileAction::Save
                  ? "Overwrite this preset?"
                  : "Load this preset?";
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted(prompt);
          ImGui::TableSetColumnIndex(1);
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.65f, 0.12f, 0.12f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                ImVec4(0.78f, 0.16f, 0.16f, 1.0f));
          ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                ImVec4(0.52f, 0.08f, 0.08f, 1.0f));
          if (ImGui::Button("Yes")) {
            if (pending_action == PendingProfileAction::Delete) {
              DeleteSavedProfile(profiles[i].id);
              deleted_this_frame = true;
            } else if (pending_action == PendingProfileAction::Save) {
              if (SaveCurrentToProfile(profiles[i].id)) {
                profile_feedback_index = static_cast<int>(i);
                std::snprintf(profile_feedback, sizeof(profile_feedback),
                              "Saved current settings to: %s",
                              profiles[i].name);
              }
            } else {
              if (LoadSavedProfile(profiles[i].id)) {
                // A loaded profile can replace the pass order. Reconcile the
                // live session map with the new order so retired sessions are
                // fenced and new ones are created before the next frame runs
                // them.
                AddonController::Get().EnsureSessions(settings.pass_order);
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
        }

        if (profile_feedback_index == static_cast<int>(i)) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextDisabled("%s", profile_feedback);
        }

        if (deleted_this_frame) {
          profile_feedback_index = -1;
          profile_feedback[0] = '\0';
          break;
        }
      }

      ImGui::EndTable();
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
// The pass section's body. The section header and tab row are drawn here
// rather than by the generic section renderer because pass selection and
// toolbar actions operate on the shared pass order and active-pass state.
// ---------------------------------------------------------------------------

namespace {

bool FeatureRowGate(void*, ctl::ValueStore& store, const std::string& section,
                    const ctl::InstanceKey& key) {
  if (section != feature::importance_mask::kSection) return true;
  return store.Read(feature::hdr::kSection, key, feature::hdr::kEnable).as_bool;
}

void DrawTabsRowAndBody(ctl::ValueStore& store, Settings& settings,
                        bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawTabsRowAndBody");
  // The section header keeps the legacy label.
  if (!PersistentSettingHeader(
          "Passes", feature::ui_state::kPassesOpen, changed,
          "Configure each pass profile. Execution order is grouped by target stage, then follows this tab order within each stage.")) {
    return;
  }

  IndentScope indent;
  ImGui::TextDisabled("Execution: Pre-DLSS -> Post-DLSS -> Backbuffer; pass order is preserved within each stage.");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Changing a pass target changes its execution group, not its position in the pass list. Status uses the same execution grouping.");
  }

  const StatusSnapshot tab_status = AddonController::Get().GetStatus();
  std::unordered_map<feature::pass::PassId, const PassStatus*> status_by_id;
  status_by_id.reserve(tab_status.passes.size());
  for (const PassStatusEntry& entry : tab_status.passes) {
    status_by_id.emplace(entry.id, &entry.status);
  }

  const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
  struct PassTabItem {
    feature::pass::PassId id = 0;
    uint32_t position = 0;
    std::string label;
    std::string tooltip;
    float width = 0.0f;
    bool enabled = true;
    bool failed = false;
  };
  std::vector<PassTabItem> tabs;
  tabs.reserve(settings.pass_order.size());
  for (uint32_t position = 0; position < settings.pass_order.size(); ++position) {
    const feature::pass::PassId id = settings.pass_order[position];
    const feature::pass::PassSnapshot pass =
        feature::pass::Read(store, feature::pass::PassKey(id));
    const char* stage_name = pass.target == 0 ? "Pre-DLSS" :
                             pass.target == 1 ? "Post-DLSS" : "Backbuffer";
    const char* stage_short = pass.target == 0 ? "Pre" :
                              pass.target == 1 ? "Post" : "BB";
    const auto status_it = status_by_id.find(id);
    const bool pass_failed = status_it != status_by_id.end() &&
                             status_it->second->failed;
    const char* state_tag = pass_failed ? " [FAIL]" : "";
    const char* state_description = pass_failed ? "failed" :
                                    pass.enabled ? "enabled" : "disabled";
    char label[128];
    if (!pass.custom_name.empty()) {
      std::snprintf(label, sizeof(label), "%s%s %s##PassTab%u",
                    pass.custom_name.c_str(), state_tag, stage_short, id);
    } else {
      std::snprintf(label, sizeof(label), "Pass %u%s %s##PassTab%u",
                    position + 1, state_tag, stage_short, id);
    }
    char default_name[24];
    std::snprintf(default_name, sizeof(default_name), "Pass %u", position + 1);
    char tip[256];
    std::snprintf(tip, sizeof(tip),
        "Select %s - target: %s - %s. Runtime groups passes by target stage.",
        default_name, stage_name, state_description);

    PassTabItem item;
    item.id = id;
    item.position = position;
    item.label = label;
    item.tooltip = tip;
    item.enabled = pass.enabled;
    item.failed = pass_failed;
    item.width = ImGui::GetFrameHeight() + spacing_x +
                 ImGui::CalcTextSize(item.label.c_str()).x +
                 ImGui::GetStyle().FramePadding.x * 2.0f;
    tabs.push_back(std::move(item));
  }

  // Draw tabs and toolbar in the parent window using normal ImGui item flow.
  // Child windows must not overlap the item hit regions: even a visually
  // transparent child can intercept clicks from controls beneath it.
  const float row_width = ImGui::GetContentRegionAvail().x;
  const ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  const ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
  const ImVec4 inactive_color = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

  float line_used = 0.0f;
  for (const PassTabItem& tab : tabs) {
    const float checkbox_width = ImGui::GetFrameHeight();
    const float desired_button_width = tab.width - checkbox_width - spacing_x;
    const float button_width = std::min(desired_button_width,
                                        std::max(1.0f, row_width - checkbox_width - spacing_x));
    const float item_width = checkbox_width + spacing_x + button_width;
    if (line_used > 0.0f &&
        line_used + spacing_x + item_width <= row_width) {
      ImGui::SameLine();
      line_used += spacing_x + item_width;
    } else {
      line_used = item_width;
    }

    const bool is_active = settings.active_pass_id == tab.id;
    ImGui::PushID(static_cast<int>(tab.id));
    bool pass_enabled = tab.enabled;
    if (ImGui::Checkbox("##PassTabEnabled", &pass_enabled)) {
      store.Write(feature::pass::kSection, feature::pass::PassKey(tab.id),
                  feature::pass::kPassEnabled, ctl::Value::Bool(pass_enabled));
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Toggle this pass. Current state: %s.", pass_enabled ? "enabled" : "disabled");
    }
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        tab.enabled ? 1.0f : ImGui::GetStyle().Alpha * 0.55f);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          is_active ? active_color : inactive_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_color);
    if (ImGui::Button(tab.label.c_str(), ImVec2(button_width, 0.0f))) {
      settings.active_pass_id = tab.id;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("%s", tab.tooltip.c_str());
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopID();
  }

  // Explicitly end the tab layout before emitting the toolbar row.
  ImGui::NewLine();
  DrawPassToolbar(settings, changed);
  ImGui::PopStyleVar();

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

  SectionDivider();
  local_changed = false;
  ctl::DrawSection(store, feature::hdr::kSection, key, local_changed,
                   MakeOptions(/*headers=*/true, /*indent=*/true));
  changed |= local_changed;
  SectionDivider();
  local_changed = false;
  ctl::DrawOptions mask_options = MakeOptions(/*headers=*/true, /*indent=*/true);
  mask_options.row_gate = &FeatureRowGate;
  ctl::DrawSection(store, feature::importance_mask::kSection, key,
                   local_changed, mask_options);
  changed |= local_changed;
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
  DLSSNR_PROFILE_UI_L1("DrawOverlay");
  Settings& settings = GetSettings();
  bool changed = false;

  ctl::ValueStore& store = feature::SettingsStore();
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

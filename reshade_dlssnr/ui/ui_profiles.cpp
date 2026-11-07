#include "ui/ui_sections.h"
#include "profiling/profile.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "feature/ui_state/settings.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_widgets.h"

#define LOC_NAMESPACE "ui"

namespace reshade_dlssnr {

namespace {

// The action column is sized from the widest button set, so both rows share
// one width and the table does not jitter when a row switches to renaming.
const localization::TextRef kRename = LOC_TEXT("profiles.rename", "Rename");
const localization::TextRef kLoad = LOC_TEXT("profiles.load", "Load");
const localization::TextRef kSave = LOC_TEXT("profiles.save", "Save");
const localization::TextRef kRemove = LOC_TEXT("profiles.remove", "x");
const localization::TextRef kSaveName =
    LOC_TEXT("profiles.save_name", "Save Name");
const localization::TextRef kCancel = LOC_TEXT("profiles.cancel", "Cancel");
const localization::TextRef kSaveAsNew =
    LOC_TEXT("profiles.save_as_new", "Save as New Preset");
const localization::TextRef kNameError = LOC_TEXT(
    "profiles.name_error", "Preset name is blank, too long, or already in use.");

}  // namespace

void DrawSavedProfilesSection(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawSavedProfilesSection");
  DrawCollapsible(
      LOC_TEXT("profiles", "Presets"), feature::ui_state::kPresetsOpen, changed,
      LOC_TEXT("profiles.tip",
               "Save and restore named profiles of the current DLSSNR settings "
               "in ReShade.ini."),
      [&] {
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
    ImGui::TextDisabled("%s",
                        LOC_TEXT("profiles.none", "No saved presets.").c_str());
  } else {
    // Fixed-width action column so a rename row (Save Name / Cancel) and a
    // normal row (Rename / Load / Save / x) share the same column width and
    // do not make the table jitter when switching modes. Computed from the
    // wider of the two button sets.
    const float frame_pad = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_normal =
        ImGui::CalcTextSize(kRename.c_str()).x +
        ImGui::CalcTextSize(kLoad.c_str()).x +
        ImGui::CalcTextSize(kSave.c_str()).x +
        ImGui::CalcTextSize(kRemove.c_str()).x +
        4.0f * frame_pad + 3.0f * spacing;
    const float row_rename =
        ImGui::CalcTextSize(kSaveName.c_str()).x +
        ImGui::CalcTextSize(kCancel.c_str()).x +
        2.0f * frame_pad + spacing;
    const float action_width =
        row_normal > row_rename ? row_normal : row_rename;

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    if (ImGui::BeginTable("##saved_profiles", 2, table_flags)) {
      ImGui::TableSetupColumn(LOC_TEXT("profiles.column.preset", "Preset"),
                              ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn(LOC_TEXT("profiles.column.actions", "Actions"),
                              ImGuiTableColumnFlags_WidthFixed, action_width);

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
            Tip(LOC_TEXT("profiles.rename.tip",
                "Enter saves the name; Escape cancels editing."));
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
                                     : LOC_TEXT("profiles.unnamed",
                                                "(unnamed preset)")
                                           .c_str());
        }

        // Actions column. While renaming, only Save Name and Cancel are
        // shown: Load / Save / x would act on a preset whose name is still
        // being edited, and hiding them gives the row a single meaning.
        ImGui::TableSetColumnIndex(1);
        if (is_renaming) {
          if (ImGui::Button(kSaveName.c_str())) rename_commit = true;
          ImGui::SameLine();
          if (ImGui::Button(kCancel.c_str())) rename_cancel = true;
        } else {
          if (ImGui::Button(kRename.c_str())) {
            renaming_profile_id = profiles[i].id;
            std::snprintf(rename_profile_name, sizeof(rename_profile_name),
                          "%s", profiles[i].name);
            rename_profile_error = false;
            rename_focus_pending = true;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip(LOC_TEXT("profiles.rename.button_tip",
                "Edit this preset's name. Enter saves; Escape "
                "cancels."));
          }
          ImGui::SameLine();
          if (ImGui::Button(kLoad.c_str())) {
            pending_action = PendingProfileAction::Load;
            pending_profile_id = profiles[i].id;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip(LOC_TEXT("profiles.load.tip",
                "Load this preset into the active settings and save "
                "it as the current ReShade.ini configuration."));
          }
          ImGui::SameLine();
          if (ImGui::Button(kSave.c_str())) {
            pending_action = PendingProfileAction::Save;
            pending_profile_id = profiles[i].id;
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip(LOC_TEXT("profiles.save.tip",
                "Overwrite this saved preset with the current "
                "settings."));
          }
          ImGui::SameLine();
          if (ImGui::Button(kRemove.c_str())) {
            if (pending_action == PendingProfileAction::Delete &&
                pending_profile_id == profiles[i].id) {
              clear_pending();
            } else {
              pending_action = PendingProfileAction::Delete;
              pending_profile_id = profiles[i].id;
            }
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            Tip(LOC_TEXT("profiles.remove.tip",
                "Remove this preset after confirming."));
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
                          LOC_TEXT("profiles.feedback.renamed",
                                   "Renamed preset to: %s"),
                          rename_profile_name);
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
          ImGui::TextDisabled("%s", kNameError.c_str());
        }

        if (pending_profile_id == profiles[i].id &&
            pending_action != PendingProfileAction::None) {
          const char* prompt =
              pending_action == PendingProfileAction::Delete
                  ? LOC_TEXT("profiles.confirm.remove", "Remove this preset?")
                        .c_str()
              : pending_action == PendingProfileAction::Save
                  ? LOC_TEXT("profiles.confirm.overwrite",
                             "Overwrite this preset?")
                        .c_str()
                  : LOC_TEXT("profiles.confirm.load", "Load this preset?")
                        .c_str();
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
          if (ImGui::Button(LOC_TEXT("profiles.confirm.yes", "Yes"))) {
            if (pending_action == PendingProfileAction::Delete) {
              DeleteSavedProfile(profiles[i].id);
              deleted_this_frame = true;
            } else if (pending_action == PendingProfileAction::Save) {
              if (SaveCurrentToProfile(profiles[i].id)) {
                profile_feedback_index = static_cast<int>(i);
                std::snprintf(profile_feedback, sizeof(profile_feedback),
                              LOC_TEXT("profiles.feedback.saved",
                                       "Saved current settings to: %s"),
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
                              LOC_TEXT("profiles.feedback.loaded",
                                       "Loaded preset: %s"),
                              profiles[i].name);
              }
            }
            clear_pending();
          }
          ImGui::PopStyleColor(3);
          if (!deleted_this_frame) {
            ImGui::SameLine();
            if (ImGui::Button(LOC_TEXT("profiles.confirm.no", "No"))) {
              clear_pending();
            }
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
  const float save_button_width =
      ImGui::CalcTextSize(kSaveAsNew.c_str()).x +
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
  if (ImGui::Button(kSaveAsNew.c_str()) && new_profile_name[0] != '\0') {
    if (SaveCurrentAsProfile(new_profile_name)) {
      std::snprintf(profile_feedback, sizeof(profile_feedback),
                    LOC_TEXT("profiles.feedback.created",
                             "Saved new preset: %s"),
                    new_profile_name);
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
    TipFormat(LOC_TEXT("profiles.save_as_new.tip",
                 "Save all current DLSSNR settings under a new name. Up to %u "
                 "presets can be stored in ReShade.ini."), kMaxSavedProfiles);
  }
  if (profile_feedback_index == -2) ImGui::TextDisabled("%s", profile_feedback);
  if (profile_name_error) {
    ImGui::TextDisabled("%s", kNameError.c_str());
  }
  if (profile_count >= kMaxSavedProfiles) ImGui::EndDisabled();
  });
}

}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

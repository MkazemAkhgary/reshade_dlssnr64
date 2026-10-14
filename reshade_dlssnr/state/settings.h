// Persistent user settings for model passes, UI masking, and debug views.
#pragma once

#include <cstdint>

#include <reshade.hpp>

#include <string>
#include <vector>

#include "compose/settings.h"
#include "feature/pass/ui.h"
#include "grade/settings.h"
#include "project_constants.h"

namespace reshade_dlssnr {

enum class InjectionStage : int {
  PreDLSS = 0,
  PostDLSS = 1,
  Backbuffer = 2,
};

struct Settings {
  bool enabled = true;

  // The passes, in tab order. Each entry is a stable pass id, not a position:
  // reordering permutes this vector and moves nothing in the store, so a pass
  // keeps its settings however the user rearranges the tabs. Adding a pass
  // appends a fresh id; removing one erases it, and that id stays retired --
  // its settings linger unused in the store rather than being handed to a
  // pass created later.
  //
  // This is application state on purpose. ctl does not track instance count
  // and does not track instance order; the store never sees this vector.
  std::vector<feature::pass::PassId> pass_order;

  // UI state: the pass currently selected in the tab strip, held as the
  // pass's stable id. Reordering or removing another pass never changes
  // which pass is open. Transient (not persisted).
  feature::pass::PassId active_pass_id = 0;

  // UI state: which collapsible sections are open. Persisted, and shared
  // across passes — collapsing a section in one tab collapses it in every
  // tab, so the panel keeps a single, consistent shape. Model, Strength,
  // and each sub-section default to open except Advanced.
  bool ui_model_open = true;
  bool ui_strength_open = true;
  bool ui_soft_compression_open = true;
  bool ui_pass_advanced_open = false;
  bool ui_residual_open = false;
  bool ui_rcas_open = false;
  bool ui_grade_input_open = false;
  bool ui_grade_output_open = false;
  bool ui_status_open = true;
  bool ui_preferences_open = false;
  bool ui_presets_open = false;
  bool ui_passes_open = true;
  bool ui_debug_open = false;
  bool ui_hdr_open = true;
  bool ui_grade_open = true;
  bool ui_hdr_diagnostics_open = false;
  bool ui_bounded_tuning_open = false;
  bool ui_importance_mask_open = true;
};

constexpr uint32_t kMaxSavedProfiles = 16;
constexpr uint32_t kSavedProfileNameCapacity = 64;
using ProfileId = uint32_t;

struct SavedProfileInfo {
  ProfileId id = 0;
  char name[kSavedProfileNameCapacity] = {};
};

// Allocates a fresh, never-reused pass id. The high-water mark is
// persisted so a restart cannot hand out an id that is already in
// pass_order. Removing a pass retires its id for the life of the
// installation.
feature::pass::PassId NewPassId();

Settings& GetSettings();

void LoadSettings();
void SaveSettings();

ProfileId NewProfileId();

// Erase the persisted keys for one pass. Called when a pass is removed so
// that its retired id leaves no orphan keys in the config. Only the three
// pass-keyed sections (Pass, Grade Model Input, Grade Model Output) are
// touched, and only for the given id.
void ErasePassKeys(feature::pass::PassId id);

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity);
bool SaveCurrentAsProfile(const char* name);
bool SaveCurrentToProfile(ProfileId id);
bool RenameSavedProfile(ProfileId id, const char* name);
bool LoadSavedProfile(ProfileId id);
bool DeleteSavedProfile(ProfileId id);

}  // namespace reshade_dlssnr

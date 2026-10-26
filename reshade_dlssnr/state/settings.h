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

// On-disk format version for the addon's settings store. Bumped when the
// persisted tree shape changes in a way older builds would misread. This is
// independent of kAddonVersion: settings survive addon updates, so a build
// identifier is not a suitable format version.
constexpr int kSettingsVersion = 1;

enum class InjectionStage : int {
  PreDLSS = 0,
  PostDLSS = 1,
  Backbuffer = 2,
};

struct Settings {
  bool enabled = true;

  // Runtime cache of the stable pass IDs in tab order. The persisted source of
  // truth is the ctl "Pass Order" setting; state/settings keeps this vector for
  // convenient frame/UI access.
  std::vector<feature::pass::PassId> pass_order;

  // Transient UI state: the selected pass's stable ID, never persisted.
  feature::pass::PassId active_pass_id = 0;
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
void SetPassOrder(Settings& settings, feature::pass::PassOrder order);

// Erase the persisted keys for one pass. Called when a pass is removed so
// that its retired id leaves no orphan keys in the config. Every pass-keyed
// section is touched: the Pass section, its two hosted model grades, the
// four HDR sections, and Exposure Mask. Only the given id's keys are
// removed.
void ErasePassKeys(feature::pass::PassId id);

uint32_t GetSavedProfiles(SavedProfileInfo* profiles, uint32_t capacity);
bool SaveCurrentAsProfile(const char* name);
bool SaveCurrentToProfile(ProfileId id);
bool RenameSavedProfile(ProfileId id, const char* name);
bool LoadSavedProfile(ProfileId id);
bool DeleteSavedProfile(ProfileId id);

}  // namespace reshade_dlssnr

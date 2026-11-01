#include "state/migrations.h"

#include <array>

#include "feature/local_exposure/settings.h"
#include "state/settings.h"

namespace reshade_dlssnr {
namespace {

// Spellings of a shape that has left the code. A step describes what an older
// build wrote, so it cannot borrow the current names for it.
constexpr const char* kOldLocalExposureSection = "HDR Local Exposure";

// Moves one settings tree's local-exposure keys to the section that now owns
// them. The setting names are unchanged, so the whole instance map moves as it
// is.
void MoveLocalExposureSection(json::Value& s_subtree) {
  json::Value* old = s_subtree.find(kOldLocalExposureSection);
  if (old == nullptr || !old->is_object()) return;

  json::Value& moved = s_subtree[feature::local_exposure::kSection];
  for (const auto& entry : old->object()) {
    moved[entry.first] = entry.second;
  }
  s_subtree.erase(kOldLocalExposureSection);
}

// v1 -> v2: local exposure stopped being a layer of the HDR stack.
void MigrateLocalExposureOutOfHdr(json::Value& root) {
  if (json::Value* s = root.find("S")) MoveLocalExposureSection(*s);

  // A saved profile carries its own copy of the same tree, under Data.S.
  json::Value* presets = root.find("UserPreset");
  if (presets == nullptr || !presets->is_array()) return;
  for (json::Value& preset : presets->array()) {
    json::Value* data = preset.find("Data");
    if (data == nullptr) continue;
    if (json::Value* s = data->find("S")) MoveLocalExposureSection(*s);
  }
}

// Append-only. Never edit a shipped step: a file in the field may already have
// run it, so changing what it does changes what those files mean.
constexpr int kMigrationCount = 1;

constexpr std::array<SettingsMigration, kMigrationCount> kSettingsMigrations = {{
    {1, &MigrateLocalExposureOutOfHdr},
}};

// True when the table holds exactly one step per version from 1 to
// kSettingsVersion - 1, in order. A gap is a version no step can upgrade, and
// the tree would silently keep its old shape instead of failing.
constexpr bool MigrationChainComplete(
    const std::array<SettingsMigration, kMigrationCount>& steps) {
  if (kMigrationCount != kSettingsVersion - 1) return false;
  for (int version = 1; version < kSettingsVersion; ++version) {
    if (steps[static_cast<size_t>(version - 1)].from_version != version) {
      return false;
    }
  }
  return true;
}

static_assert(MigrationChainComplete(kSettingsMigrations),
              "the settings migration table must hold one step per version "
              "from 1 to kSettingsVersion - 1, ascending and contiguous");

}  // namespace

void MigrateSettingsTree(json::Value& root, int file_version) {
  for (int version = file_version; version < kSettingsVersion; ++version) {
    kSettingsMigrations[static_cast<size_t>(version - 1)].apply(root);
  }
}

}  // namespace reshade_dlssnr

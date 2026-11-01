#include "state/migrations.h"

#include <array>

#include "state/settings.h"

namespace reshade_dlssnr {
namespace {

// Append-only. Never edit a shipped step: a file in the field may already have
// run it, so changing what it does changes what those files mean.
constexpr int kMigrationCount = 0;

constexpr std::array<SettingsMigration, kMigrationCount> kSettingsMigrations = {};

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

// Ordered migration steps for the persisted settings tree.
//
// The version belongs to the file, not to a setting: a tree has no total order
// over its keys, so a per-setting version has nothing to compare against.
//
// Each step names the version it upgrades FROM and leaves the tree at the next
// one, so the table is contiguous by construction and a file at version N runs
// exactly the steps N..kSettingsVersion-1. Steps are append-only.
//
// A step rewrites the raw JSON tree, not the typed Settings struct: it has to
// keep working after the schema moves on, and the typed struct only ever
// describes the current version.
#pragma once

#include "ctl/usage/json.h"

namespace reshade_dlssnr {

struct SettingsMigration {
  // Version the tree is at before this step runs.
  int from_version;
  // Rewrites the tree in place. Runs once, on a tree known to be at
  // `from_version`.
  void (*apply)(json::Value& root);
};

// Brings `root` from `file_version` to kSettingsVersion.
//
// `file_version` must be in 1..kSettingsVersion-1. A tree from a newer build is
// the caller's to refuse: applying this build's steps to it would destroy data
// this build cannot express.
void MigrateSettingsTree(json::Value& root, int file_version);

}  // namespace reshade_dlssnr

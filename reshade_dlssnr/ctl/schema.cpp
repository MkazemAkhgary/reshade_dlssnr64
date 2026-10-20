#include "ctl/schema.h"
#include "profiling/profile.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace reshade_dlssnr {
namespace ctl {

bool Schema::Build(std::string* error) {
  DLSSNR_PROFILE_CTL_L2("ctl::Schema::Build");
  rows_.clear();
  rows_.resize(sections_.size());

  auto fail = [&](const std::string& message) {
    if (error != nullptr) *error = message;
    built_ = false;
    return false;
  };

  for (uint32_t i = 0; i < sections_.size(); ++i) {
    Section& section = *sections_[i];
    section.index_ = i;

    for (uint32_t j = 0; j < i; ++j) {
      if (sections_[j]->name() == section.name()) {
        return fail("duplicate section name: " + section.name());
      }
    }

    for (uint32_t si = 0; si < section.settings_.size(); ++si) {
      DLSSNR_PROFILE_CTL_L3("ctl setting-name validation");
      DLSSNR_PROFILE_CTL_TAG_L3("Setting",
                                section.settings_[si]->meta().name.c_str());
      const std::string& name = section.settings_[si]->meta().name;
      for (uint32_t sj = 0; sj < si; ++sj) {
        if (section.settings_[sj]->meta().name == name) {
          return fail("duplicate setting name '" + name + "' in section '" +
                      section.name() + "'");
        }
      }
    }

    if (!section.header_toggle_setting_.empty()) {
      const Setting* header_toggle = nullptr;
      for (const auto& setting : section.settings_) {
        if (setting->meta().name == section.header_toggle_setting_) {
          header_toggle = setting.get();
          break;
        }
      }
      if (header_toggle == nullptr) {
        return fail("section header toggle names unknown setting '" +
                    section.header_toggle_setting_ + "' in section '" +
                    section.name() + "'");
      }
      if (header_toggle->DefaultValue().kind != Value::Kind::Bool) {
        return fail("section header toggle must reference a bool setting '" +
                    section.header_toggle_setting_ + "' in section '" +
                    section.name() + "'");
      }
    }

    // Category toggles are also bool settings. Each category has at most one
    // toggle, so its header has an unambiguous source of truth.
    for (uint32_t si = 0; si < section.settings_.size(); ++si) {
      const SettingMeta& meta = section.settings_[si]->meta();
      if (!meta.category_toggle) continue;
      if (meta.category.empty()) {
        return fail("category toggle setting '" + meta.name +
                    "' has no category in section '" + section.name() + "'");
      }
      if (section.settings_[si]->DefaultValue().kind != Value::Kind::Bool) {
        return fail("category toggle must be a bool setting '" + meta.name +
                    "' in section '" + section.name() + "'");
      }
      for (uint32_t sj = 0; sj < si; ++sj) {
        const SettingMeta& previous = section.settings_[sj]->meta();
        if (previous.category_toggle && previous.category == meta.category) {
          return fail("multiple category toggles for '" + meta.category +
                      "' in section '" + section.name() + "'");
        }
      }
    }

    // Inline toggles are declared on their target setting. Every source must
    // resolve to a distinct boolean setting and can be used only once.
    for (uint32_t si = 0; si < section.settings_.size(); ++si) {
      DLSSNR_PROFILE_CTL_L3("ctl inline-toggle validation");
      const SettingMeta& target_meta = section.settings_[si]->meta();
      if (target_meta.inline_toggle_setting.empty()) continue;

      const Setting* toggle = nullptr;
      for (const auto& setting : section.settings_) {
        if (setting->meta().name == target_meta.inline_toggle_setting) {
          toggle = setting.get();
          break;
        }
      }
      if (toggle == nullptr ||
          target_meta.inline_toggle_setting == target_meta.name) {
        return fail("inline toggle on setting '" + target_meta.name +
                    "' references an unknown or identical setting in section '" +
                    section.name() + "'");
      }
      if (toggle->DefaultValue().kind != Value::Kind::Bool) {
        return fail("inline toggle source '" + target_meta.inline_toggle_setting +
                    "' is not boolean in section '" + section.name() + "'");
      }
      for (uint32_t prior = 0; prior < si; ++prior) {
        if (section.settings_[prior]->meta().inline_toggle_setting ==
            target_meta.inline_toggle_setting) {
          return fail("inline toggle source '" + target_meta.inline_toggle_setting +
                      "' is used more than once in section '" + section.name() + "'");
        }
      }
    }

    // An enable rule naming a setting that does not exist is a declaration
    // mistake, and silently ignoring it would leave a row permanently
    // editable in a build that otherwise looks fine. Catch it here.
    for (const EnableRule& rule : section.enable_rules_) {
      if (rule.setting.empty()) continue;  // section-wide rule
      bool found = false;
      for (const auto& setting : section.settings_) {
        if (setting->meta().name == rule.setting) { found = true; break; }
      }
      if (!found) {
        return fail("enable rule targets unknown setting '" + rule.setting +
                    "' in section '" + section.name() + "'");
      }
    }
  }

  // Interleave settings and hooks into a single ordered row list per section.
  // Declaration order is preserved; hooks are spliced at their anchor.
  for (uint32_t i = 0; i < sections_.size(); ++i) {
    Section& section = *sections_[i];
    std::vector<Row>& rows = rows_[i];

    for (uint32_t si = 0; si < section.settings_.size(); ++si) {
      rows.push_back(Row{Row::Kind::Setting, si, 0});
    }

    for (uint32_t hi = 0; hi < section.hooks_.size(); ++hi) {
      const Hook& hook = section.hooks_[hi];
      uint32_t insert_at = static_cast<uint32_t>(rows.size());

      if (hook.placement != Hook::Placement::Append) {
        int anchor_row = -1;
        for (uint32_t r = 0; r < rows.size(); ++r) {
          if (rows[r].kind != Row::Kind::Setting) continue;
          if (section.settings_[rows[r].setting_index]->meta().name ==
              hook.anchor) {
            anchor_row = static_cast<int>(r);
            break;
          }
        }
        if (anchor_row < 0) {
          return fail("hook '" + hook.name + "' anchors on unknown setting '" +
                      hook.anchor + "' in section '" + section.name() + "'");
        }
        insert_at = static_cast<uint32_t>(anchor_row);
        if (hook.placement == Hook::Placement::After) ++insert_at;
      }

      Row row;
      row.kind = Row::Kind::Hook;
      row.hook_index = hi;
      rows.insert(rows.begin() + insert_at, row);
    }
  }

  built_ = true;
  if (error != nullptr) error->clear();
  return true;
}

const Section* Schema::FindSection(const std::string& name) const {
  for (const auto& section : sections_) {
    if (section->name() == name) return section.get();
  }
  return nullptr;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

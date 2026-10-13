#include "ctl/draw.h"

#include <imgui.h>

#include <string>
#include <vector>

#include "ui/ui_widgets.h"

namespace reshade_dlssnr {
namespace ctl {

using reshade_dlssnr::WithDisabled;

namespace {

bool SectionIsOpen(const DrawOptions& options, const std::string& name) {
  if (options.section_open == nullptr) return true;
  return options.section_open(options.ui_context, name);
}

void SetSectionOpen(const DrawOptions& options, const std::string& name,
                    bool open) {
  if (options.set_section_open != nullptr) {
    options.set_section_open(options.ui_context, name, open);
  }
}

bool CategoryIsOpen(const DrawOptions& options, const std::string& section,
                    const std::string& category) {
  if (options.category_open == nullptr) return true;
  return options.category_open(options.ui_context, section, category);
}

void SetCategoryOpen(const DrawOptions& options, const std::string& section,
                     const std::string& category, bool open) {
  if (options.set_category_open != nullptr) {
    options.set_category_open(options.ui_context, section, category, open);
  }
}

bool DrawHook(const Hook& hook, ValueStore& store, const DrawOptions& options,
              const std::string& section, const InstanceKey& key,
              bool& changed) {
  if (hook.draw == nullptr) return false;
  const bool before = changed;
  changed |= hook.draw(hook.context, store, options, section, key, changed);
  return changed != before;
}

bool RowEnabled(const Section& section, const Setting& setting,
                const ValueStore& store, const InstanceKey& key) {
  if (setting.enabled() != nullptr) {
    return setting.enabled()(setting.enabled_context(), store, key,
                             section.name());
  }
  const SettingMeta& meta = setting.meta();
  for (const EnableRule& rule : section.enable_rules()) {
    if (rule.enabled == nullptr) continue;
    if (rule.setting == meta.name) {
      return rule.enabled(rule.context, store, key, section.name());
    }
  }
  for (const EnableRule& rule : section.enable_rules()) {
    if (rule.enabled == nullptr) continue;
    if (rule.setting.empty()) {
      return rule.enabled(rule.context, store, key, section.name());
    }
  }
  return true;
}

bool RowVisible(const Section& section, const Setting& setting,
                const ValueStore& store, const InstanceKey& key) {
  const VisibilityPredicate visible = setting.visible();
  return visible == nullptr ||
         visible(setting.visible_context(), store, key, section.name());
}

std::vector<std::string> SplitCategoryPath(const std::string& path) {
  std::vector<std::string> result;
  size_t begin = 0;
  while (begin < path.size()) {
    const size_t separator = path.find('/', begin);
    const size_t end = separator == std::string::npos ? path.size() : separator;
    if (end > begin) result.emplace_back(path.substr(begin, end - begin));
    if (separator == std::string::npos) break;
    begin = separator + 1;
  }
  return result;
}

std::string JoinCategoryPath(const std::vector<std::string>& path,
                             size_t length) {
  std::string result;
  for (size_t i = 0; i < length; ++i) {
    if (!result.empty()) result.push_back('/');
    result += path[i];
  }
  return result;
}

const Setting* FindSetting(const Section& section, const std::string& name) {
  if (name.empty()) return nullptr;
  for (const auto& setting : section.settings()) {
    if (setting->meta().name == name) return setting.get();
  }
  return nullptr;
}

const Setting* FindCategoryToggle(const Section& section,
                                  const std::string& category_path) {
  for (const auto& setting : section.settings()) {
    const SettingMeta& meta = setting->meta();
    if (meta.category_toggle && meta.category == category_path) {
      return setting.get();
    }
  }
  return nullptr;
}

bool IsHeaderToggle(const Section& section, const Setting& setting) {
  return !section.header_toggle_setting().empty() &&
         setting.meta().name == section.header_toggle_setting();
}

bool IsCategoryToggle(const Setting& setting) {
  return setting.meta().category_toggle;
}

bool DrawCategoryHeader(const DrawOptions& options, ValueStore& store,
                        const Section& section, const InstanceKey& key,
                        const std::string& label,
                        const std::string& full_path, bool was_open,
                        const Setting* toggle_setting, bool& changed) {
  bool toggle_value = false;
  bool toggle_changed = false;
  if (toggle_setting != nullptr) {
    toggle_value = store.Read(section.name(), key,
                              toggle_setting->meta().name).as_bool;
  }

  bool open = was_open;
  if (options.category_style == DrawOptions::CategoryStyle::SubHeader) {
    const std::string id = section.name() + "/" + full_path;
    ImGui::PushID(id.c_str());
    HeaderControls controls;
    if (toggle_setting != nullptr) {
      controls.enabled = &toggle_value;
      controls.enabled_tooltip = toggle_setting->meta().tooltip.c_str();
    }
    PersistentSubHeader(label.c_str(), open, changed, nullptr, controls);
    ImGui::PopID();
    toggle_changed = toggle_setting != nullptr &&
                     toggle_value != store.Read(section.name(), key,
                         toggle_setting->meta().name).as_bool;
  } else {
    const std::string id = section.name() + "/" + full_path;
    ImGui::PushID(id.c_str());
    if (toggle_setting != nullptr) {
      if (ImGui::Checkbox("##ctl_category_toggle", &toggle_value)) {
        toggle_changed = true;
        changed = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
          !toggle_setting->meta().tooltip.empty()) {
        Tip("%s", toggle_setting->meta().tooltip.c_str());
      }
      ImGui::SameLine();
    }
    ImGui::SetNextItemOpen(was_open, ImGuiCond_Always);
    open = ImGui::TreeNodeEx(label.c_str(),
        ImGuiTreeNodeFlags_CollapsingHeader |
        ImGuiTreeNodeFlags_AllowOverlap);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Expand or collapse the %s subsection.", label.c_str());
    }
    ImGui::PopID();
  }

  if (open != was_open) {
    SetCategoryOpen(options, section.name(), full_path, open);
    changed = true;
  }
  if (toggle_changed && toggle_setting != nullptr) {
    store.Write(section.name(), key, toggle_setting->meta().name,
                Value::Bool(toggle_value), true);
    changed = true;
  }
  return open;
}

void DrawSectionToggle(const Section& section, ValueStore& store,
                       const InstanceKey& key, const Setting& setting,
                       bool& changed) {
  bool value = store.Read(section.name(), key, setting.meta().name).as_bool;
  ImGui::PushID(section.name().c_str());
  if (ImGui::Checkbox("##ctl_section_toggle", &value)) {
    store.Write(section.name(), key, setting.meta().name, Value::Bool(value),
                true);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    const std::string& tooltip = setting.meta().tooltip;
    if (!tooltip.empty()) Tip("%s", tooltip.c_str());
    else Tip("Enable or disable %s.", section.name().c_str());
  }
  ImGui::PopID();
  ImGui::SameLine();
}

}  // namespace

bool DrawSection(ValueStore& store, const std::string& section_name,
                 const InstanceKey& key, bool& changed,
                 const DrawOptions& options) {
  const Schema& schema = store.schema();
  const Section* section = schema.FindSection(section_name);
  if (section == nullptr) return false;

  InstanceSet& set_ref = store.SectionSet(*section);
  Instance& instance = set_ref.Get(key);
  const uint32_t offset = store.SettingOffsetOf(*section);
  bool local_changed = false;

  if (options.draw_headers) {
    const Setting* toggle_setting =
        FindSetting(*section, section->header_toggle_setting());
    if (toggle_setting != nullptr) {
      DrawSectionToggle(*section, store, key, *toggle_setting, local_changed);
    }

    const char* label = section->name().c_str();
    const bool was_open = SectionIsOpen(options, section->name());
    ImGui::SetNextItemOpen(was_open, ImGuiCond_Always);
    const bool open = ImGui::TreeNodeEx(
        label, ImGuiTreeNodeFlags_CollapsingHeader |
                   ImGuiTreeNodeFlags_AllowOverlap);
    if (open != was_open) {
      SetSectionOpen(options, section->name(), open);
      local_changed = true;
    }
    if (options.header_action != nullptr) {
      options.header_action(options.ui_context, section_name, local_changed);
    }
    if (!open) {
      changed |= local_changed;
      return local_changed;
    }
    if (options.indent_bodies) ImGui::Indent(16.0f);
  }

  std::vector<std::string> active_path;
  std::vector<bool> active_open;

  auto switch_path = [&](const std::vector<std::string>& desired) {
    size_t common = 0;
    while (common < active_path.size() && common < desired.size() &&
           active_path[common] == desired[common]) {
      ++common;
    }
    while (active_path.size() > common) {
      if (active_open.back()) ImGui::Unindent(16.0f);
      active_path.pop_back();
      active_open.pop_back();
    }
    for (size_t i = common; i < desired.size(); ++i) {
      const bool parent_open = active_open.empty() || active_open.back();
      bool open = false;
      if (parent_open) {
        const std::string full_path = JoinCategoryPath(desired, i + 1);
        const std::string& label = desired[i];
        const bool was_open = CategoryIsOpen(options, section_name, full_path);
        open = DrawCategoryHeader(options, store, *section, key, label,
                                  full_path, was_open,
                                  FindCategoryToggle(*section, full_path),
                                  local_changed);
      }
      active_path.push_back(desired[i]);
      active_open.push_back(open);
      if (open) ImGui::Indent(16.0f);
    }
  };

  for (const Schema::Row& row : schema.rows(*section)) {
    if (row.kind == Schema::Row::Kind::Hook) {
      const Hook& hook = section->hooks()[row.hook_index];
      if (hook.has_category) switch_path(SplitCategoryPath(hook.category));
      const bool path_open = active_open.empty() || active_open.back();
      if (!path_open) continue;
      DrawHook(hook, store, options, section->name(), key, local_changed);
      continue;
    }

    const Setting& setting = *section->settings()[row.setting_index];
    const std::string& category = setting.meta().category;
    if (!RowVisible(*section, setting, store, key)) continue;
    switch_path(SplitCategoryPath(category));
    const bool path_open = active_open.empty() || active_open.back();
    if (!path_open || IsHeaderToggle(*section, setting) ||
        IsCategoryToggle(setting)) {
      continue;
    }

    const bool indented = !category.empty();
    if (indented) ImGui::Indent(16.0f);

    const Value resolved = store.Resolve(instance, offset + row.setting_index);

    SettingContext context;
    context.setting = &setting;
    context.enabled = RowEnabled(*section, setting, store, key) &&
                      (options.row_gate == nullptr ||
                       options.row_gate(options.ui_context, store,
                                        section_name, key));

    Value written = resolved;
    bool row_changed = false;
    WithDisabled(!context.enabled, [&] {
      row_changed = setting.Draw(context, resolved, written);
    });
    if (row_changed) {
      store.Write(section_name, key, setting.meta().name, written,
                  /*from_user=*/true);
      local_changed = true;
    }

    if (indented) ImGui::Unindent(16.0f);
  }

  switch_path({});
  if (options.draw_headers && options.indent_bodies) ImGui::Unindent(16.0f);

  changed |= local_changed;
  return local_changed;
}

}  // namespace ctl
}  // namespace reshade_dlssnr

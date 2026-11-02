#include "ctl/menu.h"

#include <imgui.h>

#include <string>
#include <utility>
#include <vector>

#include "ctl/draw.h"
#include "ctl/schema.h"

namespace reshade_dlssnr {
namespace ctl {

namespace {

constexpr const char* kPopupId = "##ctl_context_menu";

const Setting* FindSetting(const Section& section, const std::string& name) {
  for (const auto& setting : section.settings()) {
    if (setting->meta().name == name) return setting.get();
  }
  return nullptr;
}

bool IsInCategory(const std::string& category, const std::string& scope) {
  if (scope.empty()) return true;
  if (category == scope) return true;
  return category.size() > scope.size() &&
         category.compare(0, scope.size(), scope) == 0 &&
         category[scope.size()] == '/';
}

// Whether the target spans this setting. One rule for both directions: what
// Copy takes and what Paste is willing to apply.
bool InScope(const Setting& setting, const MenuTarget& target) {
  if (target.setting != nullptr) return &setting == target.setting;
  return IsInCategory(setting.meta().category, target.category);
}

// The values `target` spans, read from the instance `from`.
void Collect(const ValueStore& store, const MenuTarget& target,
             const InstanceKey& from, Clipboard& out) {
  out.clear();

  auto append = [&](const Section& section) {
    ClipboardSection payload;
    payload.section = section.name();
    for (const auto& setting : section.settings()) {
      if (!InScope(*setting, target)) continue;
      payload.entries.push_back(
          {setting->meta().name, store.Read(payload.section, from,
                                            setting->meta().name)});
    }
    if (!payload.entries.empty()) out.sections.push_back(std::move(payload));
  };

  if (target.section != nullptr) {
    append(*target.section);
    return;
  }

  // A whole instance: every instanced section that holds one at this key.
  for (const auto& section : store.schema().sections()) {
    if (!section->instanced()) continue;
    if (store.SectionSet(*section).Find(from) == nullptr) continue;
    append(*section);
  }
}

// One clipboard entry the target takes, with the value it would be written as.
struct PasteItem {
  const ClipboardSection* payload;
  const ClipboardEntry* entry;
  Value value;
};

std::vector<PasteItem> Accepted(const ValueStore& store,
                                const MenuTarget& target,
                                const Clipboard& clipboard) {
  std::vector<PasteItem> items;
  for (const ClipboardSection& payload : clipboard.sections) {
    if (target.section != nullptr && payload.section != target.section->name()) {
      continue;
    }
    const Section* section = store.schema().FindSection(payload.section);
    if (section == nullptr) continue;
    for (const ClipboardEntry& entry : payload.entries) {
      const Setting* setting = FindSetting(*section, entry.setting);
      if (setting == nullptr || !InScope(*setting, target)) continue;
      Value value;
      if (!setting->Assign(value, entry.value)) continue;
      setting->Clamp(value);
      items.push_back({&payload, &entry, value});
    }
  }
  return items;
}

bool Paste(ValueStore& store, const MenuTarget& target,
           const Clipboard& clipboard) {
  bool changed = false;
  for (const PasteItem& item : Accepted(store, target, clipboard)) {
    const std::string& section = item.payload->section;
    const std::string& setting = item.entry->setting;
    if (store.Read(section, target.key, setting) == item.value) continue;
    if (store.Write(section, target.key, setting, item.value, true)) {
      changed = true;
    }
  }
  return changed;
}

void DrawBuiltins(ValueStore& store, const DrawOptions& options,
                  const MenuTarget& target, bool& changed) {
  Clipboard& clipboard = *options.clipboard;
  if (ImGui::MenuItem("Copy")) Collect(store, target, target.key, clipboard);

  const bool can_paste =
      !clipboard.empty() && !Accepted(store, target, clipboard).empty();
  if (ImGui::MenuItem("Paste", nullptr, false, can_paste)) {
    changed |= Paste(store, target, clipboard);
  }
}

void DrawCommands(const MenuCommandRegistry* registry, ValueStore& store,
                  const MenuTarget& target, bool& changed) {
  if (registry == nullptr) return;

  bool first = true;
  std::string group;
  for (const MenuCommand& command : registry->commands()) {
    if (command.execute == nullptr) continue;
    if (command.visible != nullptr &&
        !command.visible(command.context, store, target)) {
      continue;
    }
    if (first || command.group != group) {
      ImGui::Separator();
      first = false;
      group = command.group;
    }
    const bool enabled =
        command.enabled == nullptr || command.enabled(command.context, store,
                                                      target);
    ImGui::PushID(command.name.c_str());
    if (ImGui::MenuItem(command.label.c_str(), nullptr, false, enabled)) {
      changed |= command.execute(command.context, store, target);
    }
    ImGui::PopID();
  }
}

}  // namespace

bool MenuCommandRegistry::Add(MenuCommand command) {
  if (command.name.empty() || command.execute == nullptr) return false;
  for (const MenuCommand& existing : commands_) {
    if (existing.name == command.name) return false;
  }
  commands_.push_back(std::move(command));
  return true;
}

bool ContextMenuClicked(const Rect& rect) {
  if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
    return false;
  }
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Right)) return false;
  const ImVec2 mouse = ImGui::GetMousePos();
  return mouse.x >= rect.left && mouse.x <= rect.right &&
         mouse.y >= rect.top && mouse.y <= rect.bottom;
}

bool DrawContextMenu(ValueStore& store, const DrawOptions& options,
                     const MenuTarget& target, const Rect& rect) {
  if (options.clipboard == nullptr) return false;

  // One popup per target, so the frame's other targets do not begin this one.
  const bool named = target.section != nullptr;
  if (named) {
    if (target.setting != nullptr) {
      ImGui::PushID(target.setting->meta().name.c_str());
    } else if (!target.category.empty()) {
      ImGui::PushID(target.category.c_str());
    } else {
      ImGui::PushID("section");
    }
  }

  if (ContextMenuClicked(rect)) ImGui::OpenPopup(kPopupId);

  bool changed = false;
  if (ImGui::BeginPopup(kPopupId)) {
    DrawBuiltins(store, options, target, changed);
    DrawCommands(options.menu_commands, store, target, changed);
    ImGui::EndPopup();
  }

  if (named) ImGui::PopID();
  return changed;
}

bool CopyFromInstance(ValueStore& store, const MenuTarget& target,
                      const InstanceKey& source) {
  Clipboard clipboard;
  Collect(store, target, source, clipboard);
  if (clipboard.empty()) return false;
  return Paste(store, target, clipboard);
}

bool DrawSettingContextMenu(ValueStore& store, const DrawOptions& options,
                            const std::string& section, const InstanceKey& key,
                            const std::string& setting, const Rect& rect) {
  const Section* found = store.schema().FindSection(section);
  if (found == nullptr) return false;
  const Setting* target_setting = FindSetting(*found, setting);
  if (target_setting == nullptr) return false;
  const MenuTarget target{found, target_setting, key};
  return DrawContextMenu(store, options, target, rect);
}

}  // namespace ctl
}  // namespace reshade_dlssnr

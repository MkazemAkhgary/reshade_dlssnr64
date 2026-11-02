// ctl context menu: copy, paste, and the commands an application adds.
//
// A command is data plus three predicates, so adding one is a registration and
// not a change to the drawer. The clipboard and the registry are caller state,
// passed in DrawOptions like the open-state callbacks.

#pragma once

#include <string>
#include <vector>

#include "ctl/types.h"
#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

class Section;
class Setting;
struct DrawOptions;

// What the menu acts on. A null `section` means the whole instance -- the
// caller's own handle for it, a pass tab -- and a null `setting` means the
// section, narrowed to `category` when that is set. The target decides how
// much of the clipboard a paste applies.
struct MenuTarget {
  const Section* section = nullptr;
  const Setting* setting = nullptr;
  InstanceKey key;
  std::string category;  // category path within the section; empty == all
};

// A rectangle in ImGui's screen space.
struct Rect {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;
};

struct ClipboardEntry {
  std::string setting;
  Value value;
};

struct ClipboardSection {
  std::string section;
  std::vector<ClipboardEntry> entries;
};

// What Copy put aside. A single value is a one-entry clipboard; the target a
// paste lands on decides how much of it is applied.
struct Clipboard {
  std::vector<ClipboardSection> sections;

  bool empty() const { return sections.empty(); }
  void clear() { sections.clear(); }
};

using MenuPredicate = bool (*)(const void* context, const ValueStore& store,
                               const MenuTarget& target);
using MenuExecute = bool (*)(const void* context, ValueStore& store,
                             const MenuTarget& target);

// One entry the application contributes to every context menu.
struct MenuCommand {
  std::string name;   // stable identity; no two commands may share one
  std::string group;  // entries of one group are drawn together
  std::string label;
  MenuPredicate visible = nullptr;  // null means always visible
  MenuPredicate enabled = nullptr;  // null means always enabled
  MenuExecute execute = nullptr;
  const void* context = nullptr;
};

class MenuCommandRegistry {
 public:
  // Returns false and leaves the registry unchanged when `command` has no
  // name, no execute, or reuses a name.
  bool Add(MenuCommand command);

  const std::vector<MenuCommand>& commands() const { return commands_; }

 private:
  std::vector<MenuCommand> commands_;
};

// True when the mouse right-clicked inside `rect` this frame.
bool ContextMenuClicked(const Rect& rect);

// Draw the menu for `target`, opening it when `rect` is right-clicked. Must be
// called every frame the target is drawn, or ImGui closes the popup. Returns
// true when a command wrote a value.
bool DrawContextMenu(ValueStore& store, const DrawOptions& options,
                     const MenuTarget& target, const Rect& rect);

// The same, for one named setting of a section. For a caller that draws a row
// itself, like a hook, and does not hold the Setting.
bool DrawSettingContextMenu(ValueStore& store, const DrawOptions& options,
                            const std::string& section, const InstanceKey& key,
                            const std::string& setting, const Rect& rect);

// Copy the values `target` spans from the instance `source` into its own
// instance. Returns true when a value changed.
bool CopyFromInstance(ValueStore& store, const MenuTarget& target,
                      const InstanceKey& source);

}  // namespace ctl
}  // namespace reshade_dlssnr

// ctl rendering: draw a schema into an ImGui window.
//
// Rendering follows declaration order exactly. Hooks are drawn inline at
// their anchor position. Inherited-vs-overridden decoration is applied only
// to settings that opted into inheritance; every other setting draws exactly
// as it would in a section with no parent at all.

#pragma once

#include <cstdint>
#include <string>

#include "ctl/schema.h"
#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

struct DrawOptions {
  using HeaderAction = void (*)(void* context, const std::string& section,
                               bool& changed);
  using RowGate = bool (*)(void* context, ValueStore& store,
                           const std::string& section, const InstanceKey& key);
  // Section open/closed state, keyed by section name. The drawer owns no
  // persistent UI state of its own; the caller supplies it.
  bool (*section_open)(void* context, const std::string& section) = nullptr;
  void (*set_section_open)(void* context, const std::string& section,
                           bool open) = nullptr;

  // Sub-group open/closed state, keyed by (section, category path). Category
  // paths use '/' to nest groups; consecutive settings with the same path are
  // drawn under that hierarchy of collapsible sub-headers. Unset means every
  // group is drawn open, which is the default for sections without categories.
  bool (*category_open)(void* context, const std::string& section,
                        const std::string& category) = nullptr;
  void (*set_category_open)(void* context, const std::string& section,
                            const std::string& category, bool open) = nullptr;

  // How a category sub-header is styled. `plain` is a bare CollapsingHeader,
  // which is enough for a section that rarely uses categories; `subheader`
  // matches the tinted sub-heading the hand-written UI used, so a migrated
  // nested group keeps its look.
  enum class CategoryStyle { Plain, SubHeader };
  CategoryStyle category_style = CategoryStyle::Plain;

  void* ui_context = nullptr;

  // Optional application-owned control placed at the right edge of a section
  // header (for example, a reset action). The section toggle itself remains
  // native to ctl and is driven by Section::HeaderToggle.
  HeaderAction header_action = nullptr;

  // Optional application-owned gate combined with each row's declared gate.
  // The header and its toggle stay interactive when a body row is gated off.
  RowGate row_gate = nullptr;

  // Draw the section header at all. Off for sections the caller inlines.
  bool draw_headers = true;

  // Indent section bodies. Off when the caller already indented.
  bool indent_bodies = true;
};

// Draw one section for one instance. Returns true when anything changed.
// The store is non-const because drawing a row can write an override and
// revert one; both go through the store's own Write/Revert.
//
// There is deliberately no "draw every section" entry point. Which sections
// appear, and in what order, is a property of the page drawing them -- and a
// page that folds some sections into a tab bar (HDR's layers) must not also
// let them fall out of a blanket iteration. The caller enumerates.
bool DrawSection(ValueStore& store, const std::string& section_name,
                 const InstanceKey& key, bool& changed,
                 const DrawOptions& options = {});

}  // namespace ctl
}  // namespace reshade_dlssnr

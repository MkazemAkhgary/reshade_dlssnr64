#include <imgui.h>

#include "ui.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ctl/draw.h"
#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/debug_log/settings.h"
#include "feature/grade/settings.h"
#include "feature/grade/view.h"
#include "feature/hdr/settings.h"
#include "feature/exposure_mask/settings.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/preferences/settings.h"
#include "feature/ui_state/settings.h"
#include "feature/ui_state/view.h"
#include "hooks/dlss_guides.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"
#include "version.h"

namespace reshade_dlssnr {

// The overlay's per-frame scratch state. It is handed to the framework as an
// opaque pointer so the open-state callbacks can share one context.
namespace {

// The caller's opaque handle. The open-state callbacks below ignore it and
// read the global settings directly, but DrawOptions still threads it through,
// so it needs to be something.
void* Ui() {
  static int token = 0;
  return &token;
}

// ---------------------------------------------------------------------------
// Section open/closed state is stored as ordinary global ctl settings.
// ---------------------------------------------------------------------------

const char* SectionOpenSetting(const std::string& name) {
  if (name == feature::preferences::kSection) return feature::ui_state::kPreferencesOpen;
  if (name == feature::debug_log::kSection) return feature::ui_state::kDebugOpen;
  if (name == feature::grade::kSection) return feature::ui_state::kGradeOpen;
  if (name == feature::pass::kGradeInputSection) return feature::ui_state::kGradeInputOpen;
  if (name == feature::pass::kGradeOutputSection) return feature::ui_state::kGradeOutputOpen;
  if (name == feature::hdr::kSection) return feature::ui_state::kHdrOpen;
  if (name == feature::hdr::kGameSection) return feature::ui_state::kHdrGameOpen;
  if (name == feature::hdr::kAdaptiveSection) return feature::ui_state::kHdrAdaptiveOpen;
  if (name == feature::hdr::kLocalSection) return feature::ui_state::kHdrLocalOpen;
  if (name == feature::pass::kSection) return feature::ui_state::kPassesOpen;
  if (name == feature::exposure_mask::kSection) return feature::ui_state::kExposureMaskOpen;
  return nullptr;
}

bool ReadSectionOpen(void* /*context*/, const std::string& name) {
  const char* setting = SectionOpenSetting(name);
  return setting != nullptr ? feature::ui_state::Read(setting) : true;
}

void WriteSectionOpen(void* /*context*/, const std::string& name, bool open) {
  const char* setting = SectionOpenSetting(name);
  if (setting != nullptr) feature::ui_state::Write(setting, open);
}

const char* CategoryOpenSetting(const std::string& section,
                                const std::string& category) {
  if (section == feature::hdr::kSection) {
    if (category == "Advanced") return feature::ui_state::kHdrAdvancedOpen;
    if (category == "Diagnostics") return feature::ui_state::kHdrDiagnosticsOpen;
    return nullptr;
  }
  if (section == feature::hdr::kLocalSection &&
      category == "Advanced") {
    return feature::ui_state::kHdrLocalAdvancedOpen;
  }
  if (section == feature::hdr::kAdaptiveSection &&
      category == "Filters") {
    return feature::ui_state::kHdrAdaptiveFiltersOpen;
  }
  if (section != feature::pass::kSection) return nullptr;
  if (category == "Model") return feature::ui_state::kModelOpen;
  if (category == "Strength") return feature::ui_state::kStrengthOpen;
  if (category == "Temporal Reprojection") return feature::ui_state::kTemporalOpen;
  if (category == "Residual Edits") return feature::ui_state::kResidualOpen;
  return nullptr;
}

bool ReadCategoryOpen(void* /*context*/, const std::string& section,
                      const std::string& category) {
  const char* setting = CategoryOpenSetting(section, category);
  return setting != nullptr ? feature::ui_state::Read(setting) : true;
}

void WriteCategoryOpen(void* /*context*/, const std::string& section,
                       const std::string& category, bool open) {
  const char* setting = CategoryOpenSetting(section, category);
  if (setting != nullptr) feature::ui_state::Write(setting, open);
}

ctl::DrawOptions MakeOptions(bool headers, bool indent) {
  ctl::DrawOptions options;
  options.section_open = &ReadSectionOpen;
  options.set_section_open = &WriteSectionOpen;
  options.category_open = &ReadCategoryOpen;
  options.set_category_open = &WriteCategoryOpen;
  options.category_style = ctl::DrawOptions::CategoryStyle::SubHeader;
  options.ui_context = Ui();
  options.draw_headers = headers;
  options.indent_bodies = indent;
  return options;
}

}  // namespace

// ---------------------------------------------------------------------------
// Pass-order mutation. Stable pass IDs remain attached to their stored values;
// an order change permutes the vector and nothing else.
// ---------------------------------------------------------------------------

// Reset the backbuffer grade to its declared defaults. Writes through the
// store rather than into a mirror struct: the store is the only owner of the
// value, and the header's local copy is refreshed from it on the next frame.
void ResetBackbufferGrade(void* /*context*/) {
  const ctl::InstanceKey key = ctl::InstanceKey::Shared();
  feature::SettingsStore().ResetSectionToDefault(feature::grade::kSection, key);
}

// Reorder the pass_order vector. `order` holds the source position of each
// destination position. Nothing in the store moves: each pass's values are
// keyed by its stable id.
void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  DLSSNR_PROFILE_UI_L2("ApplyPassReorder");
  const size_t total = settings.pass_order.size();
  feature::pass::PassOrder reordered(total);
  for (size_t i = 0; i < total; ++i) {
    const int source = order[i];
    if (source < 0 || static_cast<size_t>(source) >= total) continue;  // defensive
    reordered[i] = settings.pass_order[source];
  }
  SetPassOrder(settings, std::move(reordered));

  // active_pass_id is a stable id, so a reorder does not need to update it.
  // Fall back to the first pass only if the selected id somehow vanished.
  if (std::find(settings.pass_order.begin(), settings.pass_order.end(),
                settings.active_pass_id) == settings.pass_order.end()) {
    settings.active_pass_id =
        settings.pass_order.empty() ? 0 : settings.pass_order.front();
  }

  // Keep the runtime's session map in step with the new order.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// Insert a new pass after the selected one. The inserted pass gets a fresh
// stable id, so its settings start empty and every other pass keeps its own.
void AddPass(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("AddPass");
  const size_t total = settings.pass_order.size();
  if (total >= kMaxPasses) return;
  // active_pass_id is an opaque id, not an index; resolve its position.
  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  const size_t insert_at =
      active_it == settings.pass_order.end()
          ? total
          : static_cast<size_t>(std::distance(settings.pass_order.begin(),
                                              active_it)) + 1;

  feature::pass::PassOrder order = settings.pass_order;
  const feature::pass::PassId new_id = NewPassId();
  order.insert(order.begin() + static_cast<ptrdiff_t>(insert_at), new_id);
  SetPassOrder(settings, std::move(order));
  settings.active_pass_id = new_id;

  // Create the new pass's session so it is ready the first frame it runs.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// Remove the selected pass. Its id leaves the order and is never reused, so
// its settings stay in the store unread rather than being handed to a pass
// created later.
void RemoveActivePass(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("RemoveActivePass");
  if (settings.pass_order.size() <= 1) return;
  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  if (active_it == settings.pass_order.end()) return;
  const size_t remove_at =
      static_cast<size_t>(std::distance(settings.pass_order.begin(), active_it));

  // The id has to be captured before the erase: once it leaves pass_order it
  // is not reachable from anywhere else, and its keys are no longer rewritten
  // by Save, so they must be erased from the config explicitly.
  const feature::pass::PassId removed_id = *active_it;

  feature::pass::PassOrder order = settings.pass_order;
  order.erase(order.begin() + static_cast<ptrdiff_t>(remove_at));
  SetPassOrder(settings, std::move(order));

  if (settings.pass_order.empty()) {
    settings.active_pass_id = 0;
  } else {
    const size_t next_index =
        std::min(remove_at, settings.pass_order.size() - 1);
    settings.active_pass_id = settings.pass_order[next_index];
  }

  // Erase the removed pass's ctl keys so the retired id leaves no orphan
  // keys behind in the config.
  ErasePassKeys(removed_id);

  // EnsureSessions removes it from the live map and defers releasing its NGX
  // feature/resources until work recorded by earlier frames has completed.
  AddonController::Get().EnsureSessions(settings.pass_order);
  changed = true;
}

// ---------------------------------------------------------------------------
// The pass section's body. The section header and tab row are drawn here
// rather than by the generic section renderer because pass selection and
// toolbar actions operate on the shared pass order and active-pass state.
// ---------------------------------------------------------------------------

namespace {

bool FeatureRowGate(void*, ctl::ValueStore& store, const std::string& section,
                    const ctl::InstanceKey& key) {
  if (section != feature::exposure_mask::kSection) return true;
  return store.Read(feature::hdr::kSection, key, feature::hdr::kEnable).as_bool;
}

void DrawTabsRowAndBody(ctl::ValueStore& store, Settings& settings,
                        bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawTabsRowAndBody");
  // The section header keeps the legacy label.
  DrawCollapsible(
      "Passes", feature::ui_state::kPassesOpen, changed,
      "Configure each pass profile. Execution order is grouped by target stage, then follows this tab order within each stage.",
      [&] {
    ImGui::TextDisabled("Execution: Pre-DLSS -> Post-DLSS -> Backbuffer; pass order is preserved within each stage.");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Changing a pass target changes its execution group, not its position in the pass list. Status uses the same execution grouping.");
  }

  const StatusSnapshot tab_status = AddonController::Get().GetStatus();
  std::unordered_map<feature::pass::PassId, const PassStatus*> status_by_id;
  status_by_id.reserve(tab_status.passes.size());
  for (const PassStatusEntry& entry : tab_status.passes) {
    status_by_id.emplace(entry.id, &entry.status);
  }

  const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
  struct PassTabItem {
    feature::pass::PassId id = 0;
    uint32_t position = 0;
    std::string label;
    std::string tooltip;
    float width = 0.0f;
    bool enabled = true;
    bool failed = false;
  };
  std::vector<PassTabItem> tabs;
  tabs.reserve(settings.pass_order.size());
  for (uint32_t position = 0; position < settings.pass_order.size(); ++position) {
    const feature::pass::PassId id = settings.pass_order[position];
    const feature::pass::PassSnapshot pass =
        feature::pass::Read(store, feature::pass::PassKey(id));
    const char* stage_name = pass.target == 0 ? "Pre-DLSS" :
                             pass.target == 1 ? "Post-DLSS" : "Backbuffer";
    const char* stage_short = pass.target == 0 ? "Pre" :
                              pass.target == 1 ? "Post" : "BB";
    const auto status_it = status_by_id.find(id);
    const bool pass_failed = status_it != status_by_id.end() &&
                             status_it->second->failed;
    const char* state_tag = pass_failed ? " [FAIL]" : "";
    const char* state_description = pass_failed ? "failed" :
                                    pass.enabled ? "enabled" : "disabled";
    char label[128];
    if (!pass.custom_name.empty()) {
      std::snprintf(label, sizeof(label), "%s%s %s##PassTab%u",
                    pass.custom_name.c_str(), state_tag, stage_short, id);
    } else {
      std::snprintf(label, sizeof(label), "Pass %u%s %s##PassTab%u",
                    position + 1, state_tag, stage_short, id);
    }
    char default_name[24];
    std::snprintf(default_name, sizeof(default_name), "Pass %u", position + 1);
    char tip[256];
    std::snprintf(tip, sizeof(tip),
        "Select %s - target: %s - %s. Runtime groups passes by target stage.",
        default_name, stage_name, state_description);

    PassTabItem item;
    item.id = id;
    item.position = position;
    item.label = label;
    item.tooltip = tip;
    item.enabled = pass.enabled;
    item.failed = pass_failed;
    item.width = ImGui::GetFrameHeight() + spacing_x +
                 ImGui::CalcTextSize(item.label.c_str()).x +
                 ImGui::GetStyle().FramePadding.x * 2.0f;
    tabs.push_back(std::move(item));
  }

  // Draw tabs and toolbar in the parent window using normal ImGui item flow.
  // Child windows must not overlap the item hit regions: even a visually
  // transparent child can intercept clicks from controls beneath it.
  const float row_width = ImGui::GetContentRegionAvail().x;
  const ImVec4 active_color = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  const ImVec4 hovered_color = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
  const ImVec4 inactive_color = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);

  float line_used = 0.0f;
  for (const PassTabItem& tab : tabs) {
    const float checkbox_width = ImGui::GetFrameHeight();
    const float desired_button_width = tab.width - checkbox_width - spacing_x;
    const float button_width = std::min(desired_button_width,
                                        std::max(1.0f, row_width - checkbox_width - spacing_x));
    const float item_width = checkbox_width + spacing_x + button_width;
    if (line_used > 0.0f &&
        line_used + spacing_x + item_width <= row_width) {
      ImGui::SameLine();
      line_used += spacing_x + item_width;
    } else {
      line_used = item_width;
    }

    const bool is_active = settings.active_pass_id == tab.id;
    ImGui::PushID(static_cast<int>(tab.id));
    bool pass_enabled = tab.enabled;
    if (ImGui::Checkbox("##PassTabEnabled", &pass_enabled)) {
      store.Write(feature::pass::kSection, feature::pass::PassKey(tab.id),
                  feature::pass::kPassEnabled, ctl::Value::Bool(pass_enabled));
      changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Toggle this pass. Current state: %s.", pass_enabled ? "enabled" : "disabled");
    }
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        tab.enabled ? 1.0f : ImGui::GetStyle().Alpha * 0.55f);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          is_active ? active_color : inactive_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_color);
    if (ImGui::Button(tab.label.c_str(), ImVec2(button_width, 0.0f))) {
      settings.active_pass_id = tab.id;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("%s", tab.tooltip.c_str());
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopID();
  }

  // Explicitly end the tab layout before emitting the toolbar row.
  ImGui::NewLine();
  DrawPassToolbar(settings, changed);
  ImGui::PopStyleVar();

  const auto active_it = std::find(settings.pass_order.begin(),
                                   settings.pass_order.end(),
                                   settings.active_pass_id);
  if (active_it == settings.pass_order.end()) return;

  // Keep the pass's own enable checkbox and name editable while it is off.
  const feature::pass::PassId active_id = *active_it;
  ImGui::PushID(static_cast<int>(active_id));
  const ctl::InstanceKey key = feature::pass::PassKey(active_id);
  bool local_changed = false;
  // Keep the body interactive while the pass is off, including its own Enable
  // checkbox. Runtime execution alone observes the pass-enabled value.
  ctl::DrawSection(store, feature::pass::kSection, key, local_changed,
                   MakeOptions(/*headers=*/false, /*indent=*/false));
  changed |= local_changed;
  ImGui::PopID();

  SectionDivider();
  local_changed = false;
  ctl::DrawSection(store, feature::hdr::kSection, key, local_changed,
                   MakeOptions(/*headers=*/true, /*indent=*/true));
  changed |= local_changed;
  SectionDivider();
  local_changed = false;
  ctl::DrawOptions mask_options = MakeOptions(/*headers=*/true, /*indent=*/true);
  mask_options.row_gate = &FeatureRowGate;
  ctl::DrawSection(store, feature::exposure_mask::kSection, key,
                   local_changed, mask_options);
  changed |= local_changed;
  });
}

void DrawSectionHeaderAction(void*, const std::string& section,
                             bool& changed) {
  if (section != feature::grade::kSection) return;
  const float right = ImGui::GetCursorPosX() +
                      ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(std::max(0.0f, right - kResetButtonWidth));
  if (ImGui::Button("R##backbuffer_grade_reset",
                    ImVec2(kResetButtonWidth, 0.0f))) {
    ResetBackbufferGrade(nullptr);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Reset all Grade Backbuffer settings to their defaults.");
  }
}

// Draw one single-instance section with the framework, keeping the legacy
// header. The identity function keeps the call site a one-liner.
void DrawOne(ctl::ValueStore& store, const char* name, bool& changed,
             ctl::DrawOptions::HeaderAction header_action = nullptr) {
  bool local_changed = false;
  ctl::DrawOptions options = MakeOptions(/*headers=*/true, /*indent=*/true);
  options.header_action = header_action;
  if (std::string(name) == feature::exposure_mask::kSection) {
    options.row_gate = &FeatureRowGate;
  }
  ctl::DrawSection(store, name, ctl::InstanceKey::Shared(), local_changed,
                   options);
  changed |= local_changed;
}


}  // namespace

void DrawOverlay(reshade::api::effect_runtime*) {
  DLSSNR_PROFILE_UI_L1("DrawOverlay");
  Settings& settings = GetSettings();
  bool changed = false;

  ctl::ValueStore& store = feature::SettingsStore();
  const ctl::InstanceKey shared_key = ctl::InstanceKey::Shared();
  g_show_tooltips = store.Read(feature::preferences::kSection, shared_key,
                               feature::preferences::kShowTooltips).as_bool;

  // Pass-order mutations are deferred until outside the tab-bar iteration.
  // This keeps ImGui's positional tab IDs and the pass order in sync.
  if (g_pending_pass_action != PendingPassAction::None) {
    const PendingPassAction action = g_pending_pass_action;
    g_pending_pass_action = PendingPassAction::None;
    g_pending_move = PendingMove::None;
    g_pending_move_index = -1;
    if (action == PendingPassAction::Add) {
      AddPass(settings, changed);
    } else if (action == PendingPassAction::Remove) {
      RemoveActivePass(settings, changed);
    }
    SaveSettings();
    changed = false;
  }

  if (g_pending_move != PendingMove::None && g_pending_move_index >= 0 &&
      g_pending_move_index <
          static_cast<int>(settings.pass_order.size())) {
    const int idx = g_pending_move_index;
    int order[kMaxPasses];
    const uint32_t total = static_cast<uint32_t>(settings.pass_order.size());
    for (uint32_t i = 0; i < total; ++i) order[i] = static_cast<int>(i);
    if (g_pending_move == PendingMove::Left && idx > 0) {
      std::swap(order[idx - 1], order[idx]);
    } else if (g_pending_move == PendingMove::Right &&
               idx + 1 < static_cast<int>(total)) {
      std::swap(order[idx], order[idx + 1]);
    }
    g_pending_move = PendingMove::None;
    ApplyPassReorder(settings, order, changed);
    SaveSettings();
    changed = false;
  }


  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("ReShade DLSSNR addon version. The panel settings are saved to the ReShade configuration.");
  }
  ImGui::Separator();

  // Master enable is a hand-drawn row: it is not part of any section.
  if (ImGui::Checkbox("Enable DLSSNR", &settings.enabled)) {
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Master switch. When off, no passes run and no DLSSNR GPU work is recorded.");
  }

  SectionDivider();

  // Status is a readout, not settings, so it stays a hook-driven section with
  // its own header and the existing DrawStatusSection.
  DrawStatusSection(settings, changed);

  SectionDivider();

  DrawOne(store, feature::preferences::kSection, changed);
  // The preference row can change the store during this draw; update the
  // tooltip gate before drawing any later controls in the same frame.
  g_show_tooltips = store.Read(feature::preferences::kSection, shared_key,
                               feature::preferences::kShowTooltips).as_bool;

  SectionDivider();

  DrawSavedProfilesSection(settings, changed);

  SectionDivider();

  DrawOne(store, feature::debug_log::kSection, changed);

  SectionDivider();

  DrawTabsRowAndBody(store, settings, changed);

  SectionDivider();

  // Grade Backbuffer uses ctl's native section-header toggle. The reset action
  // is an application-owned header action; the Enabled row is not duplicated.
  DrawOne(store, feature::grade::kSection, changed,
          &DrawSectionHeaderAction);

  SectionDivider();

  // This is intentionally refreshed from the store every overlay frame. The
  // guide layer latches the request at its next NGX evaluation, so a checkbox
  // edit is visible without reloading the addon.
  DlssGuides::Get().RequestCloneResources(
      store.Read(feature::preferences::kSection, shared_key,
                 feature::preferences::kCloneResources).as_bool);

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

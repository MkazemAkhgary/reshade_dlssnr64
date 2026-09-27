#include <imgui.h>
// For ImGuiTabBar::Tabs, DragTabId and GetCurrentTabBar, which are the only
// way to observe the result of a tab reorder. The public API offers only the
// flag that enables the drag.
#include <imgui_internal.h>

#include "ui.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include "addon_controller.h"
#include "compute/shader_params.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "settings.h"
#include "status.h"
#include "version.h"

namespace reshade_dlssnr {

namespace {

constexpr float kControlLabelWidth = 220.0f;
constexpr float kResetButtonWidth = 24.0f;

// Whether tooltips are drawn. Driven by the "Show Tooltips" checkbox and
// consulted by Tip() below, so every tooltip in this file respects the
// setting even from helpers that never see the current Settings.
bool g_show_tooltips = true;

// Variadic tooltip that no-ops when tooltips are off. All tooltip calls
// in this file route through here.
void Tip(const char* fmt, ...) {
  if (!g_show_tooltips) return;
  va_list args;
  va_start(args, fmt);
  ImGui::SetTooltipV(fmt, args);
  va_end(args);
}

// Heavier divider between top-level sections. Plain ImGui::Separator is
// used inside a section to group rows; this one, with the extra space on
// both sides, marks where one section ends and the next begins.
void SectionDivider() {
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// Row helpers
// ---------------------------------------------------------------------------

// Runs `fn` inside a disabled region when `disabled` is true.
template <typename Fn>
void WithDisabled(bool disabled, Fn&& fn) {
  if (disabled) ImGui::BeginDisabled();
  fn();
  if (disabled) ImGui::EndDisabled();
}

// A TreeNode whose open/closed state lives in a user setting, so the
// section stays as the user left it across sessions. `open_setting` is
// both the source of truth and the destination: seeded from the config on
// load, updated on user toggle, and persisted on save. Returns true when
// the section is open.
bool PersistentHeader(const char* label, bool& open_setting, bool& changed) {
  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader);
  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  return open;
}

// Scoped ImGui indentation with a smaller-than-default step, so nesting
// reads as a shift rather than a jump. Unindents on destruction, so the
// indent is balanced even when the enclosing scope has early returns.
class IndentScope {
 public:
  explicit IndentScope(float amount = 12.0f) : amount_(amount) {
    ImGui::Indent(amount_);
  }
  ~IndentScope() { ImGui::Unindent(amount_); }

  IndentScope(const IndentScope&) = delete;
  IndentScope& operator=(const IndentScope&) = delete;

 private:
  float amount_;
};

// A collapsible header for a sub-section — something nested inside a
// top-level section, e.g. "Model" inside a pass tab, a shader-param
// category, or "Diagnostics" inside HDR. Same collapse behavior as
// PersistentHeader, but with a darkened background and a dimmed, cooler
// label, so the two tiers read as distinct without changing the theme.
// The caller indents with an IndentScope; this function does not indent.
bool PersistentSubHeader(const char* label, bool& open_setting,
                         bool& changed) {
  // Text: dim, nudge blue. Relative to the current text color so it
  // survives any theme swap.
  const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  const ImVec4 tint(base.x * 0.82f, base.y * 0.82f, base.z * 0.92f, base.w);

  // Background: transparent at rest, so the tier distinction comes from
  // the text tint and the indent rather than a fill. Hover and active
  // still tint at reduced alpha so the header gives feedback. The theme's
  // own hover/active colors are reused, just dialed down.
  const ImVec4 hdr_hover = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
  const ImVec4 hdr_active = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);

  ImGui::PushStyleColor(ImGuiCol_Text, tint);
  ImGui::PushStyleColor(ImGuiCol_Header,
                        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::PushStyleColor(
      ImGuiCol_HeaderHovered,
      ImVec4(hdr_hover.x, hdr_hover.y, hdr_hover.z, hdr_hover.w * 0.30f));
  ImGui::PushStyleColor(
      ImGuiCol_HeaderActive,
      ImVec4(hdr_active.x, hdr_active.y, hdr_active.z, hdr_active.w * 0.50f));

  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_CollapsingHeader);

  ImGui::PopStyleColor(4);

  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  return open;
}

// A float slider row: label at the control column, slider filling the
// remaining width, reset button at the far right, and (for passes > 0) an
// inherit toggle after reset. When inheriting, the slider is disabled and
// shows the value resolved from earlier passes.
bool DrawFloatRow(uint32_t pass_index,
                  const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  bool* inherit_flag,
                  float inherited_value,
                  float lo,
                  float hi,
                  float default_value) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = (pass_index > 0) && (inherit_flag != nullptr);
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - trailing_width -
                          ImGui::GetStyle().ItemSpacing.x);

  const bool inheriting = has_inherit && *inherit_flag;
  float display_value = inheriting ? inherited_value : *raw_value;

  const std::string slider_id = std::string("##") + id;
  bool slider_changed = false;
  WithDisabled(inheriting, [&] {
    slider_changed =
    slider_changed =
        ImGui::SliderFloat(slider_id.c_str(), &display_value, lo, hi, "%.3f");
  });
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }
  if (slider_changed && !inheriting) {
    *raw_value = display_value;
    changed = true;
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *raw_value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered()) {
    Tip("Reset to default");
  }
  ImGui::PopID();

  if (has_inherit) {
    ImGui::SameLine();
    const std::string inherit_id = std::string(id) + "_inherit";
    ImGui::PushID(inherit_id.c_str());
    const bool on = *inherit_flag;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      *inherit_flag = !*inherit_flag;
      changed = true;
    }
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      Tip("Inherit this value from the previous pass");
    }
    ImGui::PopID();
  }

  return changed;
}

// An integer slider row, mirroring DrawFloatRow. `lo` and `hi` may be
// reversed (lo > hi) to put the smallest value on the right — ImGui
// handles this natively.
bool DrawIntRow(const char* id,
                const char* label,
                const char* tooltip,
                int* raw_value,
                int lo,
                int hi,
                int default_value) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                          kResetButtonWidth - ImGui::GetStyle().ItemSpacing.x);

  const std::string slider_id = std::string("##") + id;
  if (ImGui::SliderInt(slider_id.c_str(), raw_value, lo, hi)) {
    changed = true;
  }
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine();
  ImGui::PushID(id);
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    *raw_value = default_value;
    changed = true;
  }
  if (ImGui::IsItemHovered()) {
    Tip("Reset to default");
  }
  ImGui::PopID();

  return changed;
}

// A checkbox placed at the label column (used for standalone checkboxes and
// as the left half of composite rows).
bool DrawCheckboxAtLabel(const char* id, bool* value) {
  const std::string cb_id = std::string("##") + id;
  return ImGui::Checkbox(cb_id.c_str(), value);
}

// A combo row with per-item tooltips. `tooltips` is an array of item_count
// pointers (nullptr entries are fine) and `preview_tooltips` works the same
// way for the closed combo's hover tooltip.
bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  const char* const* item_tooltips,
                  int item_count) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);

  const int clamped = (*value >= 0 && *value < item_count) ? *value : 0;
  const char* preview = items[clamped];
  const std::string combo_id = std::string("##") + id;

  bool changed = false;
  if (ImGui::BeginCombo(combo_id.c_str(), preview)) {
    for (int i = 0; i < item_count; ++i) {
      const bool selected = (i == clamped);
      if (ImGui::Selectable(items[i], selected)) {
        if (*value != i) {
          *value = i;
          changed = true;
        }
      }
      if (ImGui::IsItemHovered() && item_tooltips != nullptr &&
          item_tooltips[i] != nullptr) {
        Tip("%s", item_tooltips[i]);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }

  // Hovering the closed combo shows the tooltip of the current selection.
  if (ImGui::IsItemHovered() && item_tooltips != nullptr &&
      item_tooltips[clamped] != nullptr) {
    Tip("%s", item_tooltips[clamped]);
  }

  return changed;
}

// A horizontal group of mutually-exclusive selection buttons. Each button
// takes an equal share of the available width; the currently-selected entry
// is highlighted. Returns the index of the button that was clicked, or -1
// if none was.
//
// `labels` and `tooltips` are arrays of `count` entries (tooltips may be
// nullptr for individual items). `disabled` may be nullptr, or an array of
// `count` flags to disable specific buttons while still showing them.
int DrawGroupButtons(const char* id,
                     const char* const* labels,
                     const char* const* tooltips,
                     int count,
                     int selected,
                     const bool* disabled) {
  if (count <= 0 || labels == nullptr) return -1;

  ImGui::PushID(id);

  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float total_spacing = spacing * static_cast<float>(count - 1);
  const float button_width = (avail - total_spacing) /
                             static_cast<float>(count);

  int clicked = -1;

  for (int i = 0; i < count; ++i) {
    if (i > 0) ImGui::SameLine(0.0f, spacing);

    const bool is_selected = (i == selected);
    const bool is_disabled = (disabled != nullptr && disabled[i]);

    if (is_disabled) ImGui::BeginDisabled();

    // Highlight the selected entry with the accent color across all three
    // button states, so the "chosen" appearance is stable regardless of
    // hover or click.
    const int pushed = is_selected ? 3 : 0;
    if (is_selected) {
      const ImVec4 accent =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImGui::PushStyleColor(ImGuiCol_Button, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
    }

    if (ImGui::Button(labels[i], ImVec2(button_width, 0.0f)) &&
        !is_selected && !is_disabled) {
      clicked = i;
    }

    if (pushed > 0) ImGui::PopStyleColor(pushed);

    if (tooltips != nullptr && tooltips[i] != nullptr &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("%s", tooltips[i]);
    }

    if (is_disabled) ImGui::EndDisabled();
  }

  ImGui::PopID();
  return clicked;
}

// A labelled group-button row: label at the control column, group filling
// the rest of the row. Updates `*value` and returns true if the selection
// changed.
bool DrawGroupRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  const char* const* labels,
                  const char* const* item_tooltips,
                  int count,
                  int* value,
                  const bool* disabled) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  if (tooltip != nullptr && ImGui::IsItemHovered()) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);

  const int current = (*value >= 0 && *value < count) ? *value : -1;
  const int clicked = DrawGroupButtons(id, labels, item_tooltips, count,
                                       current, disabled);
  if (clicked >= 0) {
    *value = clicked;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Section: Target
// ---------------------------------------------------------------------------

void DrawTargetSection(Settings& settings, uint32_t pass_index,
                       bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  // A pass can be at DLSS Output only if every earlier pass is too.
  bool can_be_at_dlss = true;
  for (uint32_t j = 0; j < pass_index; ++j) {
    if (settings.passes[j].injection_stage == InjectionStage::Present) {
      can_be_at_dlss = false;
      break;
    }
  }

  static const char* kTargetLabels[] = {"DLSS Output", "Backbuffer"};
  static const char* kTargetTips[] = {
      "Run this pass on the DLSS upscaled image, before the game applies "
      "UI and tone mapping.\n"
      "Unavailable when an earlier pass targets the Backbuffer, because "
      "passes run in order and later stages cannot reach back to earlier "
      "ones.",
      "Run this pass on the final frame, after every other effect "
      "including UI and post-processing.\n"
      "Selecting this forces all later passes to Backbuffer as well, "
      "because they cannot run earlier than this one."};
  const bool kTargetDisabled[] = {!can_be_at_dlss, false};

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Target");
  if (ImGui::IsItemHovered()) {
    Tip(
        "Where this pass runs in the frame.\n"
        "  DLSS Output — runs right after the game's DLSS upscaling, "
        "before UI and tone mapping are applied. Recommended: this is "
        "the cleanest image the model can see.\n"
        "  Backbuffer — runs at the very end of the frame, on the "
        "finished image.");
  }
  ImGui::SameLine(kControlLabelWidth);

  const int selected =
      pass.injection_stage == InjectionStage::PostDLSS ? 0 : 1;
  const int clicked = DrawGroupButtons("target", kTargetLabels, kTargetTips,
                                       2, selected, kTargetDisabled);

  if (clicked == 0) {
    pass.injection_stage = InjectionStage::PostDLSS;
    changed = true;
  } else if (clicked == 1) {
    pass.injection_stage = InjectionStage::Present;
    // Once a pass targets the backbuffer, all later passes must too.
    for (uint32_t j = pass_index + 1; j < kMaxPasses; ++j) {
      settings.passes[j].injection_stage = InjectionStage::Present;
    }
    changed = true;
  }
}

// ---------------------------------------------------------------------------
// Section: Pipeline (Working Scale)
// ---------------------------------------------------------------------------

void DrawPipelineSection(uint32_t pass_index,
                         Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  changed |= DrawFloatRow(
      pass_index, "working_scale", "Working Scale",
      "The resolution the model runs at, as a fraction of the frame.\n"
      "Lower values run the model faster but with less detail. "
      "1.0 runs at full resolution (slowest, best quality).",
      &pass.working_scale, &pass.inherit_working_scale,
      resolved.working_scale, 0.10f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Section: Status
// ---------------------------------------------------------------------------

// True when at least one enabled pass runs at DLSS Output, i.e. when the
// shared HDR stage would actually be applied. HDR only affects that path.
bool AnyPassUsesDlssOutput(const Settings& settings) {
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (settings.pass_enabled[i] &&
        settings.passes[i].injection_stage == InjectionStage::PostDLSS) {
      return true;
    }
  }
  return false;
}

// Describes what a pass is doing right now, in one line. The injection
// stage and working scale are passed in separately because they are
// settings, not part of the runtime status.
void FormatPassStatus(const PassStatus& pass,
                      InjectionStage stage,
                      float working_scale,
                      bool post_dlss_blocked,
                      char* out, size_t out_size) {
  const char* stage_name =
      stage == InjectionStage::PostDLSS ? "DLSS Output" : "Backbuffer";

  // Suffix describing the working scale (" · 0.5x"), empty at full
  // resolution. Only meaningful while the pass is enabled.
  char scale[24] = "";
  if (pass.enabled && working_scale < 0.999f) {
    std::snprintf(scale, sizeof(scale), " · %.3gx",
                  static_cast<double>(working_scale));
  }

  if (!pass.enabled) {
    std::snprintf(out, out_size, "Disabled");
    return;
  }
  // A Post-DLSS pass that the hook guard skipped is not the pass's fault,
  // so it reads as blocked rather than failed. Same colouring as failed,
  // because it does mean the pass is not running.
  if (post_dlss_blocked && stage == InjectionStage::PostDLSS) {
    std::snprintf(out, out_size, "%s · blocked%s", stage_name, scale);
    return;
  }
  if (pass.failed) {
    std::snprintf(out, out_size, "%s · FAILED: %s%s",
                  stage_name,
                  pass.reason[0] != '\0' ? pass.reason : "unknown error",
                  scale);
    return;
  }
  if (!pass.has_feature) {
    std::snprintf(out, out_size, "%s · initializing%s",
                  stage_name, scale);
    return;
  }
  std::snprintf(out, out_size, "%s · running%s", stage_name, scale);
}

void DrawStatusSection(Settings& settings, bool& changed) {
  if (!PersistentHeader("Status", settings.ui_status_open, changed)) {
    return;
  }

  const StatusSnapshot snap = AddonController::Get().GetStatus();

  auto bullet = [](bool good) {
    const ImVec4 color = good ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                              : ImVec4(0.9f, 0.4f, 0.4f, 1.0f);

    // Reserve a square the size of one text line and paint a filled disc
    // inside it, so the indicator does not depend on the active font
    // carrying a bullet glyph.
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.35f,
        ImGui::GetColorU32(color));

    ImGui::SameLine();
  };

  // A single cell of a two-column status table: bullet, then "Label: value".
  auto cell = [&](bool good, const char* label, const char* value) {
    bullet(good);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s: %s", label, value);
  };

  // Build the value strings up front so they can be paired into a compact
  // two-column table.
  char dlssnr_buf[16];
  std::snprintf(dlssnr_buf, sizeof(dlssnr_buf), "%s",
                snap.enabled ? "Enabled" : "Off");

  char runtime_buf[kStatusTextLength + 32];
  if (snap.runtime_initialized) {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Initialized");
  } else if (snap.runtime_reason[0] != '\0') {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Not initialized — %s",
                  snap.runtime_reason);
  } else {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Not initialized");
  }

  char guides_buf[48];
  std::snprintf(guides_buf, sizeof(guides_buf), "%s",
                snap.guides_valid ? "Valid (depth + motion)" : "Not seen");

  const bool hdr_in_use = AnyPassUsesDlssOutput(settings);
  const bool hdr_good = !settings.hdr_enable || !hdr_in_use || snap.hdr_ready;
  char hdr_buf[96];
  if (!settings.hdr_enable) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Off");
  } else if (!hdr_in_use) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Inactive (no DLSS Output pass)");
  } else if (snap.hdr_ready) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Active · exp %.3f · white %.3f",
                  snap.hdr_exposure, snap.hdr_white_point);
  } else {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Warming up");
  }

  // Shared health, two cells per row.
  if (ImGui::BeginTable("status_shared", 2,
                        ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    cell(snap.enabled, "DLSSNR", dlssnr_buf);
    ImGui::TableNextColumn();
    cell(snap.runtime_initialized, "NGX runtime", runtime_buf);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    cell(snap.guides_valid, "DLSS guides", guides_buf);
    ImGui::TableNextColumn();
    cell(hdr_good, "HDR", hdr_buf);

    ImGui::EndTable();
  }

  // Post-DLSS block notice. Deliberately outside the two-column table so
  // it reads as an alarm rather than another data field. There is no action
  // button: the only condition that blocks Post-DLSS is the addon failing to
  // install its own D3D12 hooks, which nothing in the UI can fix.
  if (snap.post_dlss_blocked) {
    ImGui::Spacing();
    bullet(false);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(snap.post_dlss_block_reason);
  }

  // Global retry: one button resets every failed pass. Replaces the
  // per-row Retry buttons, so the status section reads as one control.
  {
    bool any_failed = false;
    for (uint32_t i = 0; i < snap.pass_count; ++i) {
      if (snap.passes[i].failed) { any_failed = true; break; }
    }

    if (any_failed) {
      ImGui::Spacing();
      if (ImGui::Button("Retry failed passes")) {
        AddonController::Get().RetryAll();
      }
      if (ImGui::IsItemHovered()) {
        Tip(
            "Clear every failed pass and rebuild its model on the next "
            "frame.");
      }
    }
  }

  ImGui::Separator();

  // Per-pass summary.
  for (uint32_t i = 0; i < snap.pass_count; ++i) {
    char buf[kStatusTextLength];
    FormatPassStatus(snap.passes[i], settings.passes[i].injection_stage,
                     GetResolvedPass(i).working_scale,
                     snap.post_dlss_blocked,
                     buf, sizeof(buf));

    char label[32];
    std::snprintf(label, sizeof(label), "Pass %u", i + 1);

    const bool stage_blocked =
        snap.post_dlss_blocked &&
        settings.passes[i].injection_stage == InjectionStage::PostDLSS;
    const bool ok = snap.passes[i].enabled && !snap.passes[i].failed &&
                    !stage_blocked;

    bullet(ok);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);

    ImGui::SameLine(kControlLabelWidth);
    ImGui::TextUnformatted(buf);
  }
}

// ---------------------------------------------------------------------------
// Section: Debug view (shared across passes)
// ---------------------------------------------------------------------------

void DrawSharedDebugView(Settings& settings, bool& changed) {
  std::vector<const char*> names;
  std::vector<const char*> tips;
  names.reserve(8);
  tips.reserve(8);

  names.push_back("Off");
  tips.push_back(
      "Disable the debug view and show the normal composed output.");

  for (uint32_t i = 0; kDebugViews[i].name != nullptr; ++i) {
    names.push_back(kDebugViews[i].name);
    tips.push_back(kDebugViews[i].tooltip);
  }

  changed |= DrawComboRow(
      "debug_view", "Debug View",
      "Replace the final image with an internal buffer, for inspecting "
      "the pipeline.\n"
      "The view is rendered by whichever pass is currently selected in "
      "the tabs below, and only takes effect while that pass is enabled.",
      &settings.debug_view, names.data(), tips.data(),
      static_cast<int>(names.size()));
}

// ---------------------------------------------------------------------------
// Section: Model
// ---------------------------------------------------------------------------

bool DrawSkinStructureRow(uint32_t pass_index,
                          PassSettings& pass,
                          const PassSettings& resolved) {
  bool changed = false;

  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("skin_structure_enabled", &pass.auto_mask);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Lets the model detect character skin and apply its edits to it "
        "separately, so faces and hands are handled more gently than "
        "the rest of the scene.");
  }

  ImGui::SameLine();
  ImGui::TextUnformatted("Skin Structure");
  ImGui::SameLine(kControlLabelWidth);

  const bool has_inherit = pass_index > 0;
  float trailing_width = kResetButtonWidth;
  if (has_inherit) {
    trailing_width += ImGui::GetStyle().ItemSpacing.x + kResetButtonWidth;
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - trailing_width -
                          ImGui::GetStyle().ItemSpacing.x);

  const bool inheriting = has_inherit && pass.inherit_skin_structure;
  const bool slider_disabled = !pass.auto_mask || inheriting;
  float display_value =
      inheriting ? resolved.skin_structure : pass.skin_structure;

  WithDisabled(slider_disabled, [&] {
    if (ImGui::SliderFloat("##skin_structure_value", &display_value,
                           0.0f, 1.0f, "%.2f") && !inheriting) {
      pass.skin_structure = display_value;
      changed = true;
    }
    if (ImGui::IsItemHovered()) {
      Tip(
          "How strongly the model's edits are applied to detected skin.\n"
          "Only available when the checkbox to the left is enabled.");
    }
  });

  ImGui::SameLine();
  ImGui::PushID("skin_structure_reset");
  WithDisabled(!pass.auto_mask, [&] {
    if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.skin_structure = 1.0f;
      changed = true;
    }
  });
  if (ImGui::IsItemHovered()) Tip("Reset to default");
  ImGui::PopID();

  if (has_inherit) {
    ImGui::SameLine();
    ImGui::PushID("skin_structure_inherit");
    const bool on = pass.inherit_skin_structure;
    if (on) {
      ImGui::PushStyleColor(ImGuiCol_Button,
                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("I", ImVec2(kResetButtonWidth, 0.0f))) {
      pass.inherit_skin_structure = !pass.inherit_skin_structure;
      changed = true;
    }
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
      Tip("Inherit this value from the previous pass");
    }
    ImGui::PopID();
  }

  return changed;
}

void DrawModelSection(uint32_t pass_index,
                      Settings& settings,
                      bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  // Indent the whole sub-section (header included), so it reads as nested
  // inside the pass tab it belongs to.
  IndentScope indent;

  if (!PersistentSubHeader("Model", settings.ui_model_open, changed)) {
    return;
  }

  // Preset row.
  {
    static const char* kPresetNames[] = {"A", "B", "C"};
    static const char* kPresetTooltips[] = {
        "Model preset A.",
        "Model preset B.",
        "Model preset C.",
    };

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    if (ImGui::IsItemHovered()) {
      Tip(
          "Which of the model's three trained presets to use. "
          "They differ in how strongly the model's edits are applied.");
    }
    ImGui::SameLine(kControlLabelWidth);

    const int clicked = DrawGroupButtons(
        "preset", kPresetNames, kPresetTooltips, 3, pass.preset, nullptr);
    if (clicked >= 0) {
      pass.preset = clicked;
      changed = true;
    }
  }

  changed |= DrawFloatRow(
      pass_index, "intensity", "Intensity",
      "How strongly the model is instructed to edit the image.\n"
      "0 turns the model's edits off entirely (only the rest of the "
      "pipeline still runs). 1 is the model's intended default.",
      &pass.intensity, &pass.inherit_intensity, resolved.intensity,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_tone", "Local Tone",
      "How much of the model's local tone adjustments to apply.\n"
      "Lower values keep more of the game's original grading.",
      &pass.local_tone, &pass.inherit_local_tone, resolved.local_tone,
      0.0f, 2.0f, 1.0f);

  changed |= DrawFloatRow(
      pass_index, "local_structure", "Local Structure",
      "How much of the model's local structure detail to apply.\n"
      "Lower values keep the game's original texture character.",
      &pass.local_structure, &pass.inherit_local_structure,
      resolved.local_structure, 0.0f, 2.0f, 1.0f);

  changed |= DrawSkinStructureRow(pass_index, pass, resolved);

  // UI Correction sits under Skin Structure: both are model-side masks
  // that affect what the edit preserves. Same checkbox-at-label style, so
  // the two read as siblings.
  ImGui::AlignTextToFramePadding();
  changed |= DrawCheckboxAtLabel("ui_correction", &pass.ui_correction);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Asks the model to reduce its edits where it detects UI elements, "
        "so menus and HUD text stay crisp.");
  }
  ImGui::SameLine();
  ImGui::TextUnformatted("UI Correction");
}

// ---------------------------------------------------------------------------
// Section: Dynamic shader parameters
// ---------------------------------------------------------------------------

int FindShaderParamIndex(const char* name) {
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    if (strcmp(kShaderParams[i].name, name) == 0) return static_cast<int>(i);
  }
  return -1;
}

void DrawShaderParamSections(uint32_t pass_index,
                             Settings& settings,
                             bool& changed) {
  PassSettings& pass = settings.passes[pass_index];
  const PassSettings& resolved = GetResolvedPass(pass_index);

  uint32_t i = 0;
  uint32_t category_index = 0;
  while (i < kShaderParamCount) {
    const char* category = kShaderParams[i].category;

    uint32_t next = i;
    while (next < kShaderParamCount &&
           strcmp(kShaderParams[next].category, category) == 0) {
      ++next;
    }

    ImGui::Separator();
    // Category order is stable in kShaderParams, so the per-category UI
    // state stays mapped correctly. If the table is reordered, entries
    // past kShaderParamCount are ignored and new ones default to open.
    bool& category_open =
        settings.ui_category_open[category_index < 16 ? category_index : 15];

    // Sub-section: same indent-and-dim treatment as "Model", so categories
    // read as sitting one level in from the pass tab's own content.
    IndentScope indent;
    const bool open = PersistentSubHeader(category, category_open, changed);
    category_index++;

    if (open) {
      for (uint32_t j = i; j < next; ++j) {
        const auto& param = kShaderParams[j];
        const std::string id = std::string(category) + "_" + param.name;

        // Dependency gate: if the gating parameter is off, disable this one.
        bool dependency_off = false;
        if (param.depends_on != nullptr) {
          const int gate = FindShaderParamIndex(param.depends_on);
          if (gate >= 0 && pass.params[gate] < 0.5f) dependency_off = true;
        }

        WithDisabled(dependency_off, [&] {
          if (param.is_checkbox) {
            bool checked = pass.params[j] > 0.5f;
            ImGui::AlignTextToFramePadding();
            changed |= DrawCheckboxAtLabel(id.c_str(), &checked);
            if (ImGui::IsItemHovered() && param.tooltip != nullptr) {
              Tip("%s", param.tooltip);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(param.name);
            pass.params[j] = checked ? 1.0f : 0.0f;
          } else {
            changed |= DrawFloatRow(
                pass_index, id.c_str(), param.name, param.tooltip,
                &pass.params[j], &pass.inherit_params[j], resolved.params[j],
                param.min_value, param.max_value, param.default_value);
          }
        });
      }
    }

    i = next;
  }
}


// ---------------------------------------------------------------------------
// Pass tab body
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Section: Colour grade
// ---------------------------------------------------------------------------

const char* const kGradeLumaModeItems[] = {"Gain", "Gamma", "Lift", "Soft"};
constexpr int kGradeLumaModeCount = 4;
const char* const kGradeLumaModeTips[] = {
    "Multiply luminance by 2^amount. The classic exposure control: an equal "
    "push either way. The grade runs in linear light, so a highlight brighter "
    "than white scales with everything else instead of being clipped.",
    "Raise luminance to a power. Black stays black and white stays white, so "
    "the effect concentrates in the midtones.",
    "Raise the black level. Each pixel moves up by the room above it, or down "
    "by the room below it when the amount is negative, so nothing crosses an "
    "end of the range.",
    "A parabolic lift, zero at both ends and strongest at mid grey. The one "
    "to reach for when a large amount would otherwise clip."};

const char* const kGradeChromaModeItems[] = {"Saturation", "Vibrance",
                                             "Chroma Gain"};
constexpr int kGradeChromaModeCount = 3;
const char* const kGradeChromaModeTips[] = {
    "Blend toward luma, so every pixel moves by the same factor. The "
    "predictable default.",
    "Weight the change by how muted a pixel already is: colours near grey "
    "move at full strength and vivid ones at half, so nothing is frozen out "
    "of the control.",
    "Scale the chroma channels only, leaving luminance exactly where it was. "
    "Use when a saturation change must not move brightness."};

// One grade's controls. `id` scopes the widget names, so the per-pass grades
// and the backbuffer grade do not collide in ImGui's ID space.
//
// The two combos are the point of the section rather than decoration: the luma
// modes are alternatives for the same job, and so are the chroma modes. Only
// the selected one runs, which is what stops two algorithms from each
// re-deriving the value the other just changed.
//
// Rows are in the order the shader applies them: levels, then shaping, then
// the luma operator, then chroma. Reading the panel top to bottom is reading
// the chain, so a level set here is already in force for the operator below
// it.
void DrawGradeSection(const char* id,
                      const char* label,
                      const char* tooltip,
                      GradeSettings& grade,
                      bool& changed) {
  ImGui::PushID(id);

  changed |= DrawCheckboxAtLabel("enabled", &grade.enabled);
  if (ImGui::IsItemHovered() && tooltip != nullptr) {
    Tip("%s", tooltip);
  }

  ImGui::SameLine();
  ImGui::TextUnformatted(label);

  WithDisabled(!grade.enabled, [&] {
    changed |= DrawFloatRow(0, "black_point", "Black Point",
                            "The linear value that becomes black. Raising it "
                            "crushes the shadows: everything below it goes to "
                            "black and the rest is stretched to fill the "
                            "range. Lowering it lifts black to a dark grey.",
                            &grade.black_point, nullptr, 0.0f,
                            kGradeBlackPointMin, kGradeBlackPointMax, 0.0f);

    changed |= DrawFloatRow(0, "white_point", "White Point",
                            "The linear value that becomes white. Lowering it "
                            "brightens the whole image rather than only the "
                            "top end; raising it pulls the highlights down.",
                            &grade.white_point, nullptr, 1.0f,
                            kGradeWhitePointMin, kGradeWhitePointMax, 1.0f);

    changed |= DrawFloatRow(0, "contrast", "Contrast",
                            "Stretches the image around mid grey. Positive "
                            "steepens, negative flattens.",
                            &grade.contrast, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    changed |= DrawFloatRow(0, "shadows", "Shadows",
                            "Scales the dark end. Fades out before it reaches "
                            "white, so it cannot wash the image out on its "
                            "own.",
                            &grade.shadows, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    changed |= DrawFloatRow(0, "highlights", "Highlights",
                            "Scales the bright end. Fades out before it "
                            "reaches black, so it cannot crush the shadows on "
                            "its own.",
                            &grade.highlights, nullptr, 0.0f, -1.0f, 1.0f,
                            0.0f);

    int luma_mode = static_cast<int>(grade.luma_mode);
    if (DrawComboRow("luma_mode", "Luma Mode",
                     "Which algorithm brightness uses. These are alternatives "
                     "to each other, not layers.",
                     &luma_mode, kGradeLumaModeItems, kGradeLumaModeTips,
                     kGradeLumaModeCount)) {
      grade.luma_mode = static_cast<GradeLumaMode>(luma_mode);
      changed = true;
    }

    changed |= DrawFloatRow(0, "luma", "Brightness",
                            "Negative darkens, positive brightens.",
                            &grade.luma, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);

    int chroma_mode = static_cast<int>(grade.chroma_mode);
    if (DrawComboRow("chroma_mode", "Chroma Mode",
                     "Which algorithm saturation uses. These are alternatives "
                     "to each other, not layers.",
                     &chroma_mode, kGradeChromaModeItems, kGradeChromaModeTips,
                     kGradeChromaModeCount)) {
      grade.chroma_mode = static_cast<GradeChromaMode>(chroma_mode);
      changed = true;
    }

    changed |= DrawFloatRow(0, "chroma", "Saturation",
                            "Negative desaturates, positive saturates.",
                            &grade.chroma, nullptr, 0.0f, -1.0f, 1.0f, 0.0f);
  });

  ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Section: Residual edit
// ---------------------------------------------------------------------------
//
// How this pass's model edit is carried onto the source image. Per pass,
// because the edit is a statement about what this pass's model produced.
//
// This used to sit in the shared HDR section as "Chroma Edit" and "Edit
// Strength", which was wrong twice over: it is not HDR-specific (the transfer
// runs whether or not HDR handling is on), and it is not global.
//
// No enable checkbox. 0 is a pass-through and 1 is the model's own magnitude,
// so the slider already has both an off position and a neutral one; a
// checkbox on top would only duplicate the 0 end of it.
void DrawResidualSection(uint32_t pass_index,
                         Settings& settings,
                         bool& changed) {
  PassSettings& pass = settings.passes[pass_index];

  IndentScope indent;
  if (!PersistentSubHeader("Residual Edit", settings.ui_residual_open,
                           changed)) {
    return;
  }

  changed |= DrawFloatRow(
      pass_index, "edit_strength", "Edit Strength",
      "Scales the model's edit before it is carried onto the source.\n"
      "0 makes the compose a pass-through: the model still runs, its edit is "
      "just not applied. 1 applies the edit at the magnitude the model "
      "produced. Above 1 amplifies it.\n"
      "The edit is applied as a per-channel ratio, which is scale-invariant, "
      "so this behaves the same whether or not HDR handling is on.",
      &pass.residual.edit_strength, nullptr, 0.0f, 0.0f, 2.0f, 1.0f);
}

void DrawPassSettingsBody(uint32_t pass_index,
                          Settings& settings,
                          bool& changed) {
  ImGui::PushID(static_cast<int>(pass_index));

  char label[32];
  std::snprintf(label, sizeof(label), "Enable Pass %u", pass_index + 1);
  changed |= ImGui::Checkbox(label, &settings.pass_enabled[pass_index]);
  if (ImGui::IsItemHovered()) {
    Tip("Turn this pass on or off.");
  }
  ImGui::Separator();

  const bool enabled = settings.pass_enabled[pass_index];
  WithDisabled(!enabled, [&] {
    DrawTargetSection(settings, pass_index, changed);
    DrawPipelineSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawModelSection(pass_index, settings, changed);
    DrawShaderParamSections(pass_index, settings, changed);
    DrawResidualSection(pass_index, settings, changed);
    ImGui::Separator();
    DrawGradeSection(
        "grade_input", "Grade Model Input",
        "Grade the image the model reads.\n"
        "The model edits whatever it is given, so this changes what it "
        "produces as well as what it sees.",
        settings.passes[pass_index].grade_input, changed);
    DrawGradeSection(
        "grade_output", "Grade Model Output",
        "Grade the image the model produced, before it is published.\n"
        "This is the model's result with a grade on top; it does not feed "
        "back into the model.",
        settings.passes[pass_index].grade_output, changed);
  });
  ImGui::PopID();
}

}  // namespace

// Permutes settings.passes, settings.pass_enabled and settings.active_pass so
// position i holds the pass that was at order[i]. Re-applies what the pass
// body assumes about positions: pass 0 is the inheritance root and has no
// flags, and a Present pass forces every later pass to Present as well.
void ApplyPassReorder(Settings& settings, const int* order, bool& changed) {
  PassSettings new_passes[kMaxPasses];
  bool new_enabled[kMaxPasses];
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    new_passes[i] = settings.passes[order[i]];
    new_enabled[i] = settings.pass_enabled[order[i]];
  }
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    settings.passes[i] = new_passes[i];
    settings.pass_enabled[i] = new_enabled[i];
  }

  // active_pass names a pass, not a position, so it follows its data.
  const int old_active = settings.active_pass;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (order[i] == old_active) {
      settings.active_pass = static_cast<int>(i);
      break;
    }
  }

  settings.passes[0].inherit_working_scale = false;
  settings.passes[0].inherit_intensity = false;
  settings.passes[0].inherit_local_tone = false;
  settings.passes[0].inherit_local_structure = false;
  settings.passes[0].inherit_skin_structure = false;
  for (uint32_t i = 0; i < kShaderParamCount; ++i) {
    settings.passes[0].inherit_params[i] = false;
  }

  for (uint32_t i = 1; i < kMaxPasses; ++i) {
    if (settings.passes[i - 1].injection_stage == InjectionStage::Present) {
      settings.passes[i].injection_stage = InjectionStage::Present;
    }
  }

  changed = true;
}

void DrawOverlay(reshade::api::effect_runtime*) {
  Settings& settings = GetSettings();
  g_show_tooltips = settings.show_tooltips;
  bool changed = false;

  ImGui::Text("Version %s", reshade_dlssnr::kAddonVersion);
  ImGui::Separator();

  changed |= ImGui::Checkbox("Enable DLSSNR", &settings.enabled);
  const bool master_hovered = ImGui::IsItemHovered();

  // Show Tooltips checkbox, right-aligned on the same row as Enable
  // DLSSNR.
  {
    const char* kTipLabel = "Show Tooltips";
    const float checkbox_w =
        ImGui::GetFrameHeight() +
        ImGui::GetStyle().ItemInnerSpacing.x +
        ImGui::CalcTextSize(kTipLabel).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetContentRegionAvail().x - checkbox_w);
    changed |= ImGui::Checkbox(kTipLabel, &settings.show_tooltips);
    if (ImGui::IsItemHovered()) {
      Tip("Show tooltips for controls in this panel.");
    }
  }

  if (master_hovered) {
    Tip(
        "Master switch. When off, no passes run and no GPU work is done.");
  }

  changed |= ImGui::Checkbox("Require DLSS", &settings.only_super_sampling);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Only run when the game is actively using DLSS upscaling.\n"
        "Turn this off to also run on frames where DLSS is not in use. "
        "Doing so is rarely useful: the model expects a DLSS result as "
        "its input.");
  }

  SectionDivider();

  // --- Debug view (shared across passes) ---
  DrawSharedDebugView(settings, changed);

  SectionDivider();

  // --- Status (shared across passes) ---
  DrawStatusSection(settings, changed);

  // HDR handling only applies to passes that target DLSS Output. The
  // section is shared, so it is live whenever any enabled pass would use
  // it — not just the currently-selected tab.
  const bool hdr_relevant = AnyPassUsesDlssOutput(settings);


  SectionDivider();

  if (ImGui::BeginTabBar("PassTabs")) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Pass %u", i + 1);
      if (ImGui::BeginTabItem(label)) {
        // The open tab determines which pass's debug view is rendered.
        settings.active_pass = static_cast<int>(i);
        DrawPassSettingsBody(i, settings, changed);
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  SectionDivider();

  // --- Colour grade (shared across passes) ---
  if (PersistentHeader("Colour Grade", settings.ui_grade_open, changed)) {
    DrawGradeSection(
        "grade_backbuffer", "Grade Backbuffer",
        "Grade the final image on the swapchain backbuffer, after every pass "
        "has run.\n"
        "This is the last step before the game presents, so it applies to "
        "whatever the passes produced. A debug view replaces the backbuffer "
        "wholesale, so it is not graded.",
        settings.grade_backbuffer, changed);
  }
  SectionDivider();

  // --- HDR section (shared across passes) ---
  if (PersistentHeader("HDR", settings.ui_hdr_open, changed)) {
    changed |= ImGui::Checkbox("Enable HDR Handling",
                               &settings.hdr_enable);
    if (ImGui::IsItemHovered()) {
      Tip(
          "Prepares HDR content for the model, which expects a "
          "display-referred SDR image.\n"
          "Exposure is set automatically from the scene's brightness, and "
          "the model's edits are mapped back into the source's HDR range on "
          "output.\n"
          "Only affects passes running at DLSS Output. Passes at Backbuffer "
          "operate on a finished image and are unaffected.");
    }

    // The checkbox above stays live regardless of which pass is selected,
    // so HDR can be toggled on before wiring up a DLSS Output pass. The
    // rest of the section is inert while no pass uses the stage.
    if (!hdr_relevant) ImGui::BeginDisabled();

    const bool hdr_on = settings.hdr_enable;

    // Live measurement feedback. Without this the HDR stage has no visible
    // effect during Phase A (measurement only, no tonemapping yet).
    if (hdr_on) {
      const HdrSnapshot snap = AddonController::Get().GetHdrSnapshot();

      const char* format_name = "SDR";
      switch (snap.resolved_format) {
        case HdrFormat::Auto: format_name = "Auto"; break;
        case HdrFormat::Sdr:  format_name = "SDR"; break;
        case HdrFormat::ScRgb: format_name = "scRGB"; break;
        case HdrFormat::Hdr10: format_name = "HDR10"; break;
      }

      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Status");
      if (ImGui::IsItemHovered()) {
        Tip(
            "Summary of the current HDR measurement.\n"
            "Format — how the DLSS output is being interpreted.\n"
            "Exposure — brightness multiplier being applied before the "
            "model sees the scene.\n"
            "White — the highlight headroom the tonemapper is targeting. "
            "Expand Diagnostics for the raw numbers.");
      }
      ImGui::SameLine(kControlLabelWidth);

      if (!hdr_relevant) {
        ImGui::TextDisabled("Inactive (no DLSS Output pass)");
      } else if (!snap.ready) {
        ImGui::TextDisabled("Waiting for first measurement...");
      } else {
        ImGui::Text("Format: %s   Exposure: %.3f   White: %.3f",
                    format_name, snap.exposure, snap.white_point);
      }
    }

    WithDisabled(!hdr_on, [&] {
      // Format group.
      {
        static const char* kFormatNames[] = {
            "Auto", "SDR", "scRGB", "HDR10"};
        static const char* kFormatTips[] = {
            "Detect the format automatically from the DLSS output.",
            "Treat the source as SDR sRGB. Use only if detection is "
            "wrong on a genuinely SDR source.",
            "Treat the source as linear scRGB (1.0 = 80 nits). Common "
            "for Windows HDR and modern games.",
            "Treat the source as HDR10 PQ (ST.2084). Common for HDR "
            "games on consoles and PC."};
        int fmt = static_cast<int>(settings.hdr_format);
        changed |= DrawGroupRow(
            "hdr_format", "Format",
            "How the source image is encoded.\n"
            "Auto detects from the DLSS output's DXGI format; override "
            "it only if the automatic choice looks wrong on your system.",
            kFormatNames, kFormatTips, 4, &fmt, nullptr);
        settings.hdr_format = static_cast<HdrFormat>(fmt);
      }

      // Range is reversed: dragging right goes toward 1 frame, matching
      // the "more = more aggressive" feel of the other sliders.
      changed |= DrawIntRow(
          "hdr_measure_interval", "Measure Interval",
          "How often the scene's brightness is measured, in frames.\n"
          "1 measures every frame — the most responsive option, and the "
          "most GPU work.\n"
          "Higher values measure less often and let exposure adjust more "
          "slowly.",
          &settings.hdr_measure_interval,
          32, 1, 4);

      changed |= DrawFloatRow(
          0, "hdr_midtone", "Midtone Target",
          "Where the scene's median brightness is mapped in SDR before the "
          "model sees it.\n"
          "Higher values brighten the image fed to the model; lower values "
          "darken it. 18% grey is a common photographic reference and the "
          "default.",
          &settings.hdr_midtone, nullptr, 0.0f,
          0.01f, 0.50f, 0.18f);

      changed |= DrawFloatRow(
          0, "hdr_white_percentile", "White Percentile",
          "Which brightness percentile in the scene defines the 'white' "
          "reference for the tonemapper.\n"
          "Higher values (closer to 1.0) treat only the very brightest "
          "pixels as white, preserving more highlight detail. Lower values "
          "roll off highlights sooner.",
          &settings.hdr_white_percentile, nullptr, 0.0f,
          0.90f, 0.999f, 0.99f);

      changed |= DrawFloatRow(
          0, "hdr_highlight_headroom", "Highlight Headroom",
          "How much extra room above the detected white point the "
          "tonemapper is allowed before highlights clip.\n"
          "Higher values keep more highlight detail but compress overall "
          "contrast slightly. Lower values roll off highlights harder and "
          "can flatten bright areas.",
          &settings.hdr_highlight_headroom, nullptr, 0.0f,
          0.25f, 16.0f, 8.0f);
    });

    // Diagnostics: raw measurement values, for scenes where the exposure
    // converges somewhere unexpected. Everything below is read-only.
    if (hdr_on) {
      // Sub-section of HDR: same indent-and-background treatment as the
      // other nested headers, so it reads as belonging to HDR rather than
      // sitting beside it.
      IndentScope indent;
      if (PersistentSubHeader("Diagnostics",
                              settings.ui_hdr_diagnostics_open, changed)) {
        const HdrSnapshot snap = AddonController::Get().GetHdrSnapshot();

        if (!snap.has_histogram) {
          ImGui::TextDisabled("Waiting for first histogram measurement...");
        } else {
          if (ImGui::BeginTable("hdr_diag", 2,
                                ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Samples: %u", snap.samples);
            ImGui::TableNextColumn();
            ImGui::Text("Frames since: %u", snap.frames_since_measure);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Median (P50): %.4f", snap.p50_luma);
            ImGui::TableNextColumn();
            ImGui::Text("White source (P%.1f): %.4f",
                        settings.hdr_white_percentile * 100.0f,
                        snap.p_hi_luma);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("Target exposure: %.3f", snap.target_exposure);
            ImGui::TableNextColumn();
            ImGui::Text("Current: %.3f", snap.exposure);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("White (post-headroom): %.3f",
                        snap.white_point *
                            std::max(settings.hdr_highlight_headroom, 0.01f));

            ImGui::EndTable();
          }

          ImGui::Separator();
          ImGui::TextUnformatted("Histogram (log2 luminance)");
          if (ImGui::IsItemHovered()) {
            Tip(
                "Distribution of brightness in the measured scene, on a "
                "log2 scale.\n"
                "Left is dark, right is bright, height is how many pixels "
                "fall in that range.\n"
                "The shape tells you how the tonemapper will behave: a "
                "narrow cluster means a low-contrast scene; a long right "
                "tail means strong highlights.");
          }

          float bins_float[256];
          float peak = 1.0f;
          for (int i = 0; i < 256; ++i) {
            bins_float[i] = static_cast<float>(snap.histogram[i]);
            if (bins_float[i] > peak) peak = bins_float[i];
          }

          ImGui::PlotHistogram("##hdr_histogram", bins_float, 256,
                               0, nullptr, 0.0f, peak,
                               ImVec2(-1.0f, 80.0f));
        }
      }
    }
    if (!hdr_relevant) ImGui::EndDisabled();
  }

  if (changed) {
    SaveSettings();
  }
}

}  // namespace reshade_dlssnr

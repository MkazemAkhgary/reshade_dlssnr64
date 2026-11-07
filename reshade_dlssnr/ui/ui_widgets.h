// Shared row layout and header helpers used by the UI sections.
#pragma once

#include <cstdint>

#include <imgui.h>

#include "localization/localization.h"

namespace reshade_dlssnr {

constexpr float kControlLabelWidth = 330.0f;
constexpr float kSliderLabelWidth = 330.0f;
constexpr float kResetButtonWidth = 22.0f;

// Set from settings at the top of each frame. Read by Tip().
extern bool g_show_tooltips;

// A tooltip showing `text` as it is. The text is not a format string, so a
// translation may contain '%' without breaking.
void Tip(const localization::TextRef& text);

// A tooltip whose text carries format specifiers. The declaration says the
// string is a format, so a plain string is never mistaken for one.
void TipFormat(const char* fmt, ...);

void SectionDivider();

template <typename Fn>
void WithDisabled(bool disabled, Fn&& fn) {
  if (disabled) ImGui::BeginDisabled();
  fn();
  if (disabled) ImGui::EndDisabled();
}

class IndentScope
{
public:
    explicit IndentScope(float amount = 16.0f) : amount_(amount)
    {
        ImGui::Indent(amount_);
    }
    ~IndentScope() { ImGui::Unindent(amount_); }
    IndentScope(const IndentScope&) = delete;
    IndentScope& operator=(const IndentScope&) = delete;

private:
    float amount_;
};

using HeaderResetCallback = void (*)(void* context);

struct HeaderControls {
  bool* enabled = nullptr;
  const char* enabled_tooltip = nullptr;
  bool enabled_disabled = false;
  HeaderResetCallback reset = nullptr;
  void* reset_context = nullptr;
  float* enabled_float = nullptr;
};

// Collapsible section header whose open/closed state is a ui_state setting.
bool DrawSectionHeader(const localization::TextRef& label,
                       const char* setting_key, bool& changed,
                       const localization::TextRef& tooltip = {},
                       HeaderControls controls = {});

// Tinted sub-section header; caller owns the open/closed state.
bool DrawSubSectionHeader(const localization::TextRef& label,
                          bool& open_setting, bool& changed,
                          const localization::TextRef& tooltip = {},
                          HeaderControls controls = {});

// DrawSectionHeader plus an indented body when open.
template <typename Body>
void DrawCollapsible(const localization::TextRef& label,
                     const char* setting_key, bool& changed,
                     const localization::TextRef& tooltip, Body&& body) {
  if (!DrawSectionHeader(label, setting_key, changed, tooltip)) return;
  IndentScope indent;
  body();
}

bool DrawFloatRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  float lo,
                  float hi,
                  float default_value);

bool DrawIntRow(const char* id,
                const char* label,
                const char* tooltip,
                int* raw_value,
                int lo,
                int hi,
                int default_value);

bool DrawCheckboxAtLabel(const char* id, bool* value,
                          const char* tooltip = nullptr);

bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  const char* const* item_tooltips,
                  int item_count);

// `unavailable` marks items that can still be chosen but cannot run right now;
// they are drawn dimmed and their tooltip prefers `unavailable_reasons[i]`,
// which is runtime status text rather than a translatable label.
int DrawGroupButtons(const char* id,
                     const localization::TextRef* labels,
                     const localization::TextRef* tooltips,
                     int count,
                     int selected,
                     const bool* disabled,
                     const bool* unavailable = nullptr,
                     const char* const* unavailable_reasons = nullptr,
                     float fixed_button_width = 0.0f);

bool DrawGroupRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  const char* const* labels,
                  const char* const* item_tooltips,
                  int count,
                  int* value,
                  const bool* disabled);

}  // namespace reshade_dlssnr

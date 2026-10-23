// Shared row layout and header helpers used by the UI sections.
#pragma once

#include <cstdint>

#include <imgui.h>

namespace reshade_dlssnr {

constexpr float kControlLabelWidth = 330.0f;
constexpr float kSliderLabelWidth = 330.0f;
constexpr float kResetButtonWidth = 22.0f;

// Set from settings at the top of each frame. Read by Tip().
extern bool g_show_tooltips;

void Tip(const char* fmt, ...);
void SectionDivider();

template <typename Fn>
void WithDisabled(bool disabled, Fn&& fn) {
  if (disabled) ImGui::BeginDisabled();
  fn();
  if (disabled) ImGui::EndDisabled();
}

using HeaderResetCallback = void (*)(void* context);

struct HeaderControls {
  bool* enabled = nullptr;
  const char* enabled_tooltip = nullptr;
  bool enabled_disabled = false;
  HeaderResetCallback reset = nullptr;
  void* reset_context = nullptr;
  float* enabled_float = nullptr;
};

bool PersistentHeader(const char* label, bool& open_setting, bool& changed,
                      const char* tooltip = nullptr,
                      HeaderControls controls = {});
bool PersistentSettingHeader(const char* label, const char* setting_key,
                             bool& changed, const char* tooltip = nullptr,
                             HeaderControls controls = {});

class IndentScope {
 public:
  explicit IndentScope(float amount = 16.0f) : amount_(amount) {
    ImGui::Indent(amount_);
  }
  ~IndentScope() { ImGui::Unindent(amount_); }
  IndentScope(const IndentScope&) = delete;
  IndentScope& operator=(const IndentScope&) = delete;

 private:
  float amount_;
};

bool PersistentSubHeader(const char* label, bool& open_setting, bool& changed,
                         const char* tooltip = nullptr,
                         HeaderControls controls = {});

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
// they are drawn dimmed and their tooltip prefers `unavailable_reasons[i]`.
int DrawGroupButtons(const char* id,
                     const char* const* labels,
                     const char* const* tooltips,
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

// Shared row layout and header helpers used by the UI sections.
#pragma once

#include <cstdint>

#include <imgui.h>

namespace reshade_dlssnr {

constexpr float kControlLabelWidth = 220.0f;
constexpr float kResetButtonWidth = 24.0f;

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

bool PersistentHeader(const char* label, bool& open_setting, bool& changed);

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

bool PersistentSubHeader(const char* label, bool& open_setting, bool& changed);

bool DrawFloatRow(uint32_t pass_index,
                  const char* id,
                  const char* label,
                  const char* tooltip,
                  float* raw_value,
                  bool* inherit_flag,
                  float inherited_value,
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

bool DrawCheckboxAtLabel(const char* id, bool* value);

bool DrawComboRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  int* value,
                  const char* const* items,
                  const char* const* item_tooltips,
                  int item_count);

int DrawGroupButtons(const char* id,
                     const char* const* labels,
                     const char* const* tooltips,
                     int count,
                     int selected,
                     const bool* disabled);

bool DrawGroupRow(const char* id,
                  const char* label,
                  const char* tooltip,
                  const char* const* labels,
                  const char* const* item_tooltips,
                  int count,
                  int* value,
                  const bool* disabled);

}  // namespace reshade_dlssnr

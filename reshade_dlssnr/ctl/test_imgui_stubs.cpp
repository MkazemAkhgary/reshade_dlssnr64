// Inert ImGui entry points for the headless acceptance harness.
//
// The acceptance criteria are about values, persistence and access -- none of
// which need a live ImGui context. But the built-in setting
// rows (builtin_settings.cpp) and the drawer (draw.cpp) are the real
// implementations, and they call into ImGui. Linking the whole of ImGui pulls
// in a second copy of the widget layer and collides with ui_widgets.cpp, so
// instead this file defines just the entry points those two translation units
// reference. Every one of them returns a harmless "nothing happened" result,
// which is exactly what a test wants: it exercises the row logic without a
// real frame.
//
// This file is test scaffolding only; it is not part of the addon.

#include <imgui.h>

namespace ImGui {

// --- Layout ----------------------------------------------------------------

void BeginDisabled(bool) {}
void EndDisabled() {}
void SameLine(float, float) {}
void PushID(const char*) {}
void PushID(int) {}
void PopID() {}
void Indent(float) {}
void Unindent(float) {}
void AlignTextToFramePadding() {}
void SetNextItemWidth(float) {}
void SetNextItemDefaultFocusInCurrentWindow() {}
void SetItemDefaultFocus() {}
void SetNextItemOpen(bool, ImGuiCond) {}

ImVec2 GetContentRegionAvail() { return ImVec2(200.0f, 20.0f); }

ImGuiStyle& GetStyle() {
  static ImGuiStyle style;
  return style;
}

// --- Hover / item state ----------------------------------------------------

bool IsItemHovered(ImGuiHoveredFlags) { return false; }

// --- Widgets ---------------------------------------------------------------

bool Button(const char*, const ImVec2&) { return false; }
bool Checkbox(const char*, bool*) { return false; }
bool SliderFloat(const char*, float*, float, float, const char*, ImGuiSliderFlags) {
  return false;
}
bool SliderInt(const char*, int*, int, int, const char*, ImGuiSliderFlags) {
  return false;
}
// Inert: the harness never draws, so the stub only has to exist. It copies
// nothing, so a caller that checks the return sees "no edit".
bool InputText(const char*, char* buffer, size_t buffer_size, ImGuiInputTextFlags,
               int (*)(ImGuiInputTextCallbackData*), void*) {
  if (buffer != nullptr && buffer_size > 0) buffer[0] = '\0';
  return false;
}
bool BeginCombo(const char*, const char*, ImGuiComboFlags) { return false; }
void EndCombo() {}
bool Selectable(const char*, bool, ImGuiSelectableFlags, const ImVec2&) {
  return false;
}
bool CollapsingHeader(const char*, bool*, ImGuiTreeNodeFlags) { return false; }
bool CollapsingHeader(const char*, ImGuiTreeNodeFlags) { return false; }
void TextUnformatted(const char*, const char*) {}

}  // namespace ImGui

// ImGuiStyle's constructor is declared IMGUI_API (i.e. not inline), so the stub
// GetStyle() above needs a definition to link against. Give it sane defaults --
// the row code only reads ItemSpacing.x, and any non-zero value keeps the
// width arithmetic in the same shape the real UI produces.
ImGuiStyle::ImGuiStyle() {
  ItemSpacing = ImVec2(8.0f, 4.0f);
  ItemInnerSpacing = ImVec2(4.0f, 4.0f);
  FramePadding = ImVec2(4.0f, 3.0f);
  IndentSpacing = 21.0f;
  ScrollbarSize = 14.0f;
  _MainScale = 1.0f;
  _NextFrameFontSizeBase = 13.0f;
}

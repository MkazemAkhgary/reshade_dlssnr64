// Minimal ImGui stand-ins for the headless acceptance harness.
//
// The built-in setting rows and the drawer call into ImGui. The acceptance
// criteria are about values, persistence and access, none of
// which need a live ImGui context, so this file provides inert definitions of
// the handful of entry points the linked translation units reference. That
// keeps the harness linkable without dragging in the whole of ImGui or
// ReShade, while still compiling and exercising the real row code.
//
// This file is test scaffolding only; it is not part of the addon.

#include <imgui.h>

#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

bool g_show_tooltips = false;

void Tip(const char*, ...) {}

void SectionDivider() {}

bool PersistentHeader(const char*, bool&, bool&, const char*, HeaderControls) {
  return false;
}

bool PersistentSubHeader(const char*, bool&, bool&, const char*,
                         HeaderControls) {
  return false;
}

bool DrawFloatRow(const char*, const char*, const char*, float*,
                  float, float, float) {
  return false;
}

bool DrawIntRow(const char*, const char*, const char*, int*, int, int, int) {
  return false;
}

bool DrawCheckboxAtLabel(const char*, bool*, const char*) { return false; }

bool DrawComboRow(const char*, const char*, const char*, int*,
                  const char* const*, const char* const*, int) {
  return false;
}

int DrawGroupButtons(const char*, const char* const*, const char* const*, int,
                     int, const bool*, float) {
  return -1;
}

bool DrawGroupRow(const char*, const char*, const char*, const char* const*,
                  const char* const*, int, int*, const bool*) {
  return false;
}

}  // namespace reshade_dlssnr

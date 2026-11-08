#include <imgui.h>
#include <reshade.hpp>

#include "ui/ui_widgets.h"

#include "feature/preferences/settings.h"
#include "feature/settings_store.h"
#if DLSSNR_LOCALIZATION_ENABLED
#include "localization/overlay_font.h"
#endif
#include "feature/ui_state/view.h"
#include "ctl/types.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#define LOC_NAMESPACE "widgets"

namespace reshade_dlssnr {

bool g_show_tooltips = true;

namespace {

// Overlay language state. Only RefreshLocalization writes it.
std::string g_host_language;
std::string g_logged_host;
ImFont* g_synced_font = nullptr;
unsigned int g_synced_sources = 0;
bool g_refresh_pending = true;

// The longest tooltips in the schema are wider than the display, so they wrap
// at a fixed column, narrowed when the display is narrower than that. A
// newline in the text still starts a line.
float TooltipWrapWidth() {
  return std::min(ImGui::GetFontSize() * 35.0f,
                  ImGui::GetIO().DisplaySize.x * 0.8f);
}

// Shared by every row type that falls back to the same hint.
const localization::TextRef kResetTip = LOC_TEXT("reset.tip", "Reset to default");

std::string PreferredLanguage(DWORD flags) {
  ULONG count = 0;
  ULONG size = 0;
  if (!GetThreadPreferredUILanguages(flags, &count, nullptr, &size) ||
      size == 0) {
    return {};
  }
  std::vector<WCHAR> names(size);
  if (!GetThreadPreferredUILanguages(flags, &count, names.data(), &size) ||
      count == 0) {
    return {};
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, names.data(), -1, nullptr,
                                        0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string tag(static_cast<size_t>(bytes - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, names.data(), -1, tag.data(), bytes, nullptr,
                      nullptr);
  return tag;
}

// ReShade writes Language to the per-game ini, so check that before the global one.
std::string HostLanguage(reshade::api::effect_runtime* runtime) {
  char selected[64] = {};
  const auto read = [&](reshade::api::effect_runtime* from) {
    size_t size = sizeof(selected) - 1;
    selected[0] = '\0';
    return reshade::get_config_value(from, "OVERLAY", "Language", selected,
                                    &size) &&
           selected[0] != '\0';
  };
  if (runtime != nullptr && read(runtime)) return selected;
  if (read(nullptr)) return selected;

  // ReShade mirrors its own selection into the thread languages, so prefer
  // those over the system fallback.
  std::string tag = PreferredLanguage(MUI_LANGUAGE_NAME | MUI_THREAD_LANGUAGES);
  if (tag.empty()) {
    tag = PreferredLanguage(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK);
  }
  return tag;
}

bool OverlayHasGlyph(unsigned int codepoint, const void* font) {
  // ImWchar is 16-bit in this build.
  if (codepoint > 0xFFFF) return false;
  // IsGlyphInFont is non-const but only reads.
  ImFont* const drawable = const_cast<ImFont*>(static_cast<const ImFont*>(font));
  return drawable->IsGlyphInFont(static_cast<ImWchar>(codepoint));
}

void DrawHeaderToggle(const localization::TextRef& label, bool& changed,
                      const HeaderControls& controls) {
  DLSSNR_PROFILE_UI_L3("UI Header Toggle");
  if (controls.enabled == nullptr && controls.enabled_float == nullptr) return;
  if (controls.enabled_disabled) ImGui::BeginDisabled();

  bool float_value = controls.enabled_float != nullptr &&
                     *controls.enabled_float >= 0.5f;
  bool* checkbox_value = controls.enabled != nullptr
                             ? controls.enabled
                             : &float_value;
  if (ImGui::Checkbox("##header_enabled", checkbox_value)) {
    if (controls.enabled_float != nullptr) {
      *controls.enabled_float = float_value ? 1.0f : 0.0f;
    }
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (controls.enabled_tooltip != nullptr && controls.enabled_tooltip[0] != '\0') {
      Tip(controls.enabled_tooltip);
    } else {
      TipFormat(LOC_TEXT("toggle.tip", "Enable or disable %s."), label.c_str());
    }
  }
  if (controls.enabled_disabled) ImGui::EndDisabled();
  ImGui::SameLine();
}

void DrawHeaderReset(const localization::TextRef& label, bool& changed,
                     const HeaderControls& controls) {
  DLSSNR_PROFILE_UI_L3("UI Header Reset");
  if (controls.reset == nullptr) return;
  const float right = ImGui::GetCursorPosX() +
                       ImGui::GetContentRegionAvail().x;
  ImGui::SameLine(std::max(0.0f, right - kResetButtonWidth));
  if (ImGui::Button("R", ImVec2(kResetButtonWidth, 0.0f))) {
    controls.reset(controls.reset_context);
    changed = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    TipFormat(LOC_TEXT("reset_all.tip",
                       "Reset all %s settings to their defaults."),
              label.c_str());
  }
}

bool DrawCollapsingHeader(const localization::TextRef& label, bool& open_setting,
                          bool& changed, const localization::TextRef& tooltip,
                          HeaderControls controls, bool tinted) {
  DLSSNR_PROFILE_UI_L3("UI Collapsing Header");
  const char* text = label.c_str();

  if (tinted) {
    const ImVec4 base = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    const ImVec4 tint(base.x * 0.82f, base.y * 0.82f, base.z * 0.92f, base.w);
    const ImVec4 hdr_hover = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const ImVec4 hdr_active = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(
        ImGuiCol_HeaderHovered,
        ImVec4(hdr_hover.x, hdr_hover.y, hdr_hover.z, hdr_hover.w * 0.30f));
    ImGui::PushStyleColor(
        ImGuiCol_HeaderActive,
        ImVec4(hdr_active.x, hdr_active.y, hdr_active.z, hdr_active.w * 0.50f));
  }

  ImGui::PushID(text);
  DrawHeaderToggle(label, changed, controls);
  ImGui::SetNextItemOpen(open_setting, ImGuiCond_Always);
  const bool open =
      ImGui::TreeNodeEx(text, ImGuiTreeNodeFlags_CollapsingHeader |
                                  ImGuiTreeNodeFlags_AllowOverlap);
  const bool header_hovered =
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
  if (tinted) ImGui::PopStyleColor(4);
  if (header_hovered) {
    if (!tooltip.empty()) {
      Tip(tooltip);
    } else {
      TipFormat(LOC_TEXT("collapse.tip", "Expand or collapse the %s %s."),
                text,
                tinted ? LOC_TEXT("collapse.subsection", "subsection").c_str()
                       : LOC_TEXT("collapse.section", "section").c_str());
    }
  }
  DrawHeaderReset(label, changed, controls);
  if (open != open_setting) {
    open_setting = open;
    changed = true;
  }
  ImGui::PopID();
  return open;
}

}  // namespace

void Tip(const localization::TextRef& text) {
  DLSSNR_PROFILE_UI_L3("UI Tooltip");
  if (!g_show_tooltips || text.empty()) return;
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(TooltipWrapWidth());
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void TipFormat(const char* fmt, ...) {
  DLSSNR_PROFILE_UI_L3("UI Tooltip");
  if (!g_show_tooltips || fmt == nullptr) return;
  va_list args;
  va_start(args, fmt);
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(TooltipWrapWidth());
  ImGui::TextV(fmt, args);
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
  va_end(args);
}

namespace {

void RefreshLocalization(reshade::api::effect_runtime* runtime) {
  DLSSNR_PROFILE_UI_L3("UI Refresh Localization");
  const std::string stored =
      feature::SettingsStore()
          .Read(feature::preferences::kSection, ctl::InstanceKey::Shared(),
                feature::preferences::kLanguage)
          .as_string;
  const char* code = stored.empty() ? nullptr : stored.c_str();
  if (code == nullptr) {
    g_host_language = HostLanguage(runtime);
    code = localization::MatchLanguage(g_host_language.c_str());
    if (g_host_language != g_logged_host) {
      g_logged_host = g_host_language;
      std::string message = "DLSSNR: language auto -> ";
      message += code != nullptr ? code : "en";
      message += " (host ";
      message += g_host_language.empty() ? "none" : g_host_language.c_str();
      message += ")";
      reshade::log::message(reshade::log::level::info, message.c_str());
    }
  }
  localization::SetLanguage(code != nullptr ? code : "en");

  ImFont* font = ImGui::GetFont();
  g_synced_font = font;
  if (font == nullptr) return;
  g_synced_sources = static_cast<unsigned int>(font->Sources.Size);
  if (!localization::SyncFontCoverage(&OverlayHasGlyph, font, g_synced_sources)) {
    return;
  }
  if (localization::ActiveLanguageRenderable()) return;
  reshade::log::message(
      reshade::log::level::warning,
      "DLSSNR: the overlay font cannot draw every character in the selected "
      "language, so the overlay text falls back to English. Install a font "
      "that covers the language in the Windows Fonts folder.");
}

}  // namespace

void RequestLocalizationRefresh() { g_refresh_pending = true; }

void SyncLanguageFont(reshade::api::effect_runtime* runtime) {
#if DLSSNR_LOCALIZATION_ENABLED
  // An atlas rebuild drops the merged source, so put it back first.
  localization::EnsureOverlayFont();
#endif
  ImFont* font = ImGui::GetFont();
  if (!g_refresh_pending && font != nullptr && font == g_synced_font &&
      static_cast<unsigned int>(font->Sources.Size) == g_synced_sources) {
    return;
  }
  g_refresh_pending = false;
  RefreshLocalization(runtime);
}

void SectionDivider() {
  DLSSNR_PROFILE_UI_L3("UI Section Divider");
  ImGui::Spacing();
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::Spacing();
}

bool DrawSectionHeader(const localization::TextRef& label,
                       const char* setting_key, bool& changed,
                       const localization::TextRef& tooltip,
                       HeaderControls controls) {
  DLSSNR_PROFILE_UI_L3("UI Draw Section Header");
  bool open = feature::ui_state::Read(setting_key);
  const bool previous = open;
  const bool expanded =
      DrawCollapsingHeader(label, open, changed, tooltip, controls,
                           /*tinted=*/false);
  if (open != previous) feature::ui_state::Write(setting_key, open);
  return expanded;
}

bool DrawSubSectionHeader(const localization::TextRef& label,
                          bool& open_setting, bool& changed,
                          const localization::TextRef& tooltip,
                          HeaderControls controls) {
  return DrawCollapsingHeader(label, open_setting, changed, tooltip, controls,
                              /*tinted=*/true);
}

int DrawGroupButtons(const char* id,
                     const localization::TextRef* labels,
                     const localization::TextRef* tooltips,
                     int count,
                     int selected,
                     const bool* disabled,
                     const bool* unavailable,
                     const char* const* unavailable_reasons,
                     float fixed_button_width) {
  DLSSNR_PROFILE_UI_L3("UI Group Buttons");
  if (count <= 0 || labels == nullptr) return -1;

  ImGui::PushID(id);

  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const float avail = ImGui::GetContentRegionAvail().x;
  const float total_spacing = spacing * static_cast<float>(count - 1);
  const float distributed_width = (avail - total_spacing) /
                                  static_cast<float>(count);
  const float button_width = fixed_button_width > 0.0f
                                 ? fixed_button_width
                                 : std::max(40.0f, distributed_width);

  int clicked = -1;

  for (int i = 0; i < count; ++i) {
    if (i > 0) ImGui::SameLine(0.0f, spacing);

    const bool is_selected = (i == selected);
    const bool is_disabled = (disabled != nullptr && disabled[i]);
    const bool is_unavailable = (unavailable != nullptr && unavailable[i]);

    if (is_disabled) ImGui::BeginDisabled();
    if (is_unavailable) {
      // Still clickable, but visibly not usable right now.
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                          ImGui::GetStyle().Alpha * 0.55f);
    }

    const int pushed = is_selected ? 3 : 0;
    if (is_selected) {
      const ImVec4 accent =
          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImGui::PushStyleColor(ImGuiCol_Button, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
    }

    if (ImGui::Button(labels[i].c_str(), ImVec2(button_width, 0.0f)) &&
        !is_selected && !is_disabled) {
      clicked = i;
    }

    if (pushed > 0) ImGui::PopStyleColor(pushed);
    if (is_unavailable) {
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
    }

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      const bool has_reason = is_unavailable && unavailable_reasons != nullptr &&
                              unavailable_reasons[i] != nullptr;
      if (has_reason) {
        Tip(unavailable_reasons[i]);
      } else if (tooltips != nullptr) {
        Tip(tooltips[i]);
      } else {
        TipFormat(LOC_TEXT("select.tip", "Select %s."), labels[i].c_str());
      }
    }

    if (is_disabled) ImGui::EndDisabled();
  }

  ImGui::PopID();
  return clicked;
}

}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

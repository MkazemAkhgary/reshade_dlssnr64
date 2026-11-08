// Preferences schema, declared inline.

#include <imgui.h>

#include <string>

#include "ctl/builtin_settings.h"
#include "profiling/profile.h"
#include "ctl/draw.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "feature/preferences/settings.h"
#include "localization/localization.h"
#include "ui/ui_widgets.h"

#define LOC_NAMESPACE "prefs"

namespace reshade_dlssnr {
namespace feature {

namespace {

const localization::TextRef kAutoLanguage =
    LOC_TEXT("language.auto", "Auto (ReShade)");

bool NeverVisible(const void*, const ctl::ValueStore&,
                  const ctl::InstanceKey&, const std::string&) {
  return false;
}

#if DLSSNR_LOCALIZATION_ENABLED

// The language list is whatever the build embedded, so it cannot be an enum
// declared here. The row draws a combo over that list and stores the code.
//
// The label and tooltip come from the setting's own metadata -- the hook is
// handed it as context -- so each string is declared exactly once.
bool DrawLanguageRow(const void* context, ctl::ValueStore& store,
                     const ctl::DrawOptions&, const std::string& section,
                     const ctl::InstanceKey& key, bool& changed) {
  DLSSNR_PROFILE_UI_L3("DrawLanguageRow");
  const auto* meta = static_cast<const ctl::SettingMeta*>(context);
  const std::string current =
      store.Read(section, key, preferences::kLanguage).as_string;
  const std::vector<localization::Language>& languages =
      localization::Languages();

  // LanguageDisplayName falls back to the code when the font cannot draw the name.
  const char* preview =
      current.empty() ? kAutoLanguage.c_str() : current.c_str();
  for (size_t i = 0; i < languages.size(); ++i) {
    if (current == languages[i].code) {
      preview = localization::LanguageDisplayName(i);
      break;
    }
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(meta->label.c_str());
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip(meta->tooltip);
  }

  ImGui::SameLine(kControlLabelWidth);
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##language", preview)) {
    const bool auto_selected = current.empty();
    if (ImGui::Selectable(kAutoLanguage.c_str(), auto_selected) &&
        !auto_selected) {
      store.Write(section, key, preferences::kLanguage,
                  ctl::Value::String(preferences::kDefaultLanguage),
                  /*from_user=*/true);
      SyncLanguageFont();
      changed = true;
    }
    if (auto_selected) ImGui::SetItemDefaultFocus();

    for (size_t i = 0; i < languages.size(); ++i) {
      const bool selected = current == languages[i].code;
      if (ImGui::Selectable(localization::LanguageDisplayName(i), selected) &&
          !selected) {
        store.Write(section, key, preferences::kLanguage,
                    ctl::Value::String(languages[i].code), /*from_user=*/true);
        localization::SetLanguage(languages[i].code);
        // Re-test coverage so the rest of this frame uses the new language.
        SyncLanguageFont();
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return false;
}

#endif

}  // namespace

void RegisterPreferencesSchema(ctl::Schema& schema) {
  DLSSNR_PROFILE_CTL_L3("RegisterPreferencesSchema");
  ctl::Section& prefs = schema.AddSection(preferences::kSection, ctl::SectionKind::Single);
  prefs.Title(LOC_TEXT("title", "Preferences"));
  prefs.DefaultPersistenceBackend(ctl::PersistenceBackend::Ini);

  prefs
      .Add<ctl::BoolSetting>(preferences::kShowTooltips,
                             LOC_TEXT("show_tooltips", "Show Tooltips"), true)
      .Tooltip(LOC_TEXT("show_tooltips.tip",
                        "Show contextual descriptions when hovering controls and "
                        "section headers."));

#if DLSSNR_LOCALIZATION_ENABLED
  // Hidden from the generic drawer: the hook renders the label and the combo
  // as one row.
  ctl::StringSetting& language =
      prefs
          .Add<ctl::StringSetting>(preferences::kLanguage,
                                   LOC_TEXT("language", "Language"),
                                   preferences::kDefaultLanguage)
          .Tooltip(LOC_TEXT("language.tip",
                            "Language of the add-on interface. Auto follows "
                            "ReShade's language setting."))
          .VisibleWhen(&NeverVisible);
  prefs.AddHookAfter(preferences::kLanguage, "LanguageRow", &DrawLanguageRow,
                     &language.meta());
#endif

  prefs
      .Add<ctl::BoolSetting>(preferences::kCloneResources,
                             LOC_TEXT("clone_resources", "Clone Game Resources"),
                             false)
      .Tooltip(
          LOC_TEXT("clone_resources.tip",
                   "Off (default): read the game's current depth/motion resources and "
                   "the DLSS output directly. On: copy current-frame depth/motion, and "
                   "the DLSS output when Post-DLSS is enabled, into addon-owned "
                   "textures. This one option applies to all stages, including "
                   "Backbuffer; the requested mode is latched safely at a new DLSS "
                   "evaluation."));

  prefs
      .Add<ctl::BoolSetting>(preferences::kInvertFallbackDepth,
                             LOC_TEXT("invert_fallback_depth",
                                      "Invert Fallback Depth"),
                             false)
      .Tooltip(
          LOC_TEXT("invert_fallback_depth.tip",
                   "Invert the ReShade Generic Depth values when they are used because "
                   "the DLSS evaluation did not supply a depth resource. Does not alter "
                   "depth supplied by DLSS."));

  prefs
      .Add<ctl::BoolSetting>(preferences::kFp16Storage,
                             LOC_TEXT("fp16_storage", "FP16 Pipeline"), true)
      .Tooltip(
          LOC_TEXT("fp16_storage.tip",
                   "Store the HDR gain-map carrier, model working textures, and "
                   "unlifted composition reference in RGBA16F instead of RGBA32F. "
                   "Halves the bandwidth and footprint of the HDR pipeline at the "
                   "cost of quantization in the carrier-to-HDR multiplier and the "
                   "model-domain deltas. SDR paths are unaffected. Toggling resets "
                   "temporal history and rebuilds the NGX feature."));
}

}  // namespace feature
}  // namespace reshade_dlssnr

#undef LOC_NAMESPACE

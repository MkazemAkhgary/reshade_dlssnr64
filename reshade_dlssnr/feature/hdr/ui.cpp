// HDR non-row UI: the exposure-layer collapsibles and the diagnostics
// readout.
//
// The three layers are separate sections. Each layer header is rendered by
// PersistentSettingHeader (with the section's HeaderToggle providing the
// row-skip for the `enabled` setting) and its body by DrawSection with
// draw_headers = false. That is the whole reason the layers are sections and
// not categories: a body has to be able to draw a block of settings on its
// own, and only a section can.

#include "feature/hdr/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "ctl/draw.h"
#include "ctl/values.h"
#include "feature/hdr/settings.h"
#include "feature/ui_state/settings.h"
#include "pipeline/addon_controller.h"
#include "state/settings.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {
namespace feature {
namespace hdr {

namespace {

// Draw one layer section's body, without a header row of its own. The
// layer header is rendered by PersistentSettingHeader; the body must use
// draw_headers = false so a second header does not appear. For the same
// instance the HDR section is being drawn for. The key is forwarded whole --
// no hash, no re-derivation -- so a string-keyed instance survives the trip.
bool DrawLayerBody(ctl::ValueStore& store, const ctl::DrawOptions& options,
                   const char* section, const ctl::InstanceKey& key) {
  ctl::DrawOptions body = options;
  body.draw_headers = false;
  body.indent_bodies = false;

  bool changed = false;
  ctl::DrawSection(store, section, key, changed, body);
  return changed;
}

}  // namespace

bool DrawHdrLayers(const void* context, ctl::ValueStore& store,
                   const ctl::DrawOptions& options,
                   const std::string& section,
                   const ctl::InstanceKey& key, bool& changed) {
  (void)context;
  (void)section;
  bool local_changed = false;

  struct Layer {
    const char* label;
    const char* tooltip;
    const char* section;
    const char* open_key;
    const char* enabled_key;
  };
  const Layer layers[] = {
      {"Game Exposure",
       "Configure the game's exposure as the base layer.",
       hdr::kGameSection,
       feature::ui_state::kHdrGameOpen,
       hdr::kGameEnabled},
      {"Adaptive Exposure",
       "Histogram-based global exposure or a bounded correction over Game "
       "Exposure.",
       hdr::kAdaptiveSection,
       feature::ui_state::kHdrAdaptiveOpen,
       hdr::kAdaptiveEnabled},
      {"Local Exposure",
       "Spatially varying exposure correction applied after the global "
       "layers.",
       hdr::kLocalSection,
       feature::ui_state::kHdrLocalOpen,
       hdr::kLocalEnabled},
  };
  constexpr int kLayerCount = 3;

  for (int i = 0; i < kLayerCount; ++i) {
    // Layer on/off is a header checkbox. HeaderToggle on the section keeps
    // the drawer from rendering the same `enabled` row inside the body
    // (draw_headers = false skips header rendering, but IsHeaderToggle
    // still runs). HeaderControls drives a plain bool, so the value is
    // read from the store, mutated by the checkbox, and written back when
    // it changed -- the body's EnableWhen predicates and the runtime Read
    // both consult the store and must see the new value this same frame.
    const bool before =
        store.Read(layers[i].section, key, layers[i].enabled_key).as_bool;
    bool enabled = before;

    const char* enabled_tooltip = nullptr;
    if (const ctl::Section* layer_section =
            store.schema().FindSection(layers[i].section)) {
      for (const auto& setting : layer_section->settings()) {
        if (setting->meta().name == layers[i].enabled_key) {
          enabled_tooltip = setting->meta().tooltip.c_str();
          break;
        }
      }
    }

    HeaderControls controls;
    controls.enabled = &enabled;
    controls.enabled_tooltip = enabled_tooltip;

    bool layer_changed = false;
    const bool open = PersistentSettingHeader(
        layers[i].label, layers[i].open_key, layer_changed,
        layers[i].tooltip, controls);
    if (enabled != before) {
      store.Write(layers[i].section, key, layers[i].enabled_key,
                  ctl::Value::Bool(enabled), /*from_user=*/true);
      layer_changed = true;
    }
    local_changed |= layer_changed;
    if (open) {
      IndentScope body_indent;
      local_changed |= DrawLayerBody(store, options, layers[i].section, key);
    }
  }

  ImGui::TextDisabled(
      "Stack order: Game Exposure base, Adaptive correction, then Local "
      "Exposure. Each layer's controls are independent.");

  changed |= local_changed;
  return local_changed;
}


// BT.2446C derived k2/k4 readout. Not settings: both are computed from
// k1, k3, and the inflection point so the curve stays C1-continuous.
bool DrawBt2446Derived(const void* context, ctl::ValueStore& store,
                       const ctl::DrawOptions& options,
                       const std::string& section,
                       const ctl::InstanceKey& key, bool& changed) {
  (void)context;
  (void)options;
  (void)changed;
  const float k1 = store.Read(section, key, kBt2446K1).as_float;
  const float k3 = store.Read(section, key, kBt2446K3).as_float;
  const float ip = store.Read(section, key, kBt2446Ip).as_float;
  const float k2 = k1 * ip * (1.0f - k3);
  const float k4 = k1 * ip *
      (1.0f - (1.0f - k3) * std::log(std::max(1.0f - k3, 1e-6f)));

  IndentScope indent;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("BT.2446C k2 (derived)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Log-region slope of the BT.2446C curve. Derived from k1, k3, and "
        "the inflection point so the log branch matches the linear "
        "branch's slope at the knee.");
  }
  ImGui::SameLine(kControlLabelWidth);
  ImGui::TextDisabled("%.5f", static_cast<double>(k2));

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("BT.2446C k4 (derived)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Log-region intercept of the BT.2446C curve. Derived from k1, k3, "
        "and the inflection point so the log branch matches the linear "
        "branch's value at the knee.");
  }
  ImGui::SameLine(kControlLabelWidth);
  ImGui::TextDisabled("%.5f", static_cast<double>(k4));
  return false;
}


// HDR Diagnostics: a read-only readout of the live HDR measurement state.
//
// Everything shown here comes from the controller's own snapshots. Nothing
// in this hook writes to the store, so `changed` is only ever set by the
// collapsible header itself.
bool DrawHdrDiagnostics(const void* context, ctl::ValueStore& store,
                        const ctl::DrawOptions& options,
                        const std::string& section,
                        const ctl::InstanceKey& key, bool& changed) {
  (void)context;
  (void)store;
  (void)options;
  (void)section;
  (void)key;

  // The Diagnostics collapsible is owned by the schema (the hook carries
  // Category(kCatDiagnostics)) and persisted via kHdrDiagnosticsOpen. The
  // hook renders the body inline when the category is open; the IndentScope
  // below matches the extra indent applied to setting rows that carry a
  // category.
  IndentScope indent;
  const StatusSnapshot snap = AddonController::Get().GetStatus();
  const HdrSnapshot hdr = AddonController::Get().GetHdrSnapshot();

  const char* format_name =
      hdr.resolved_format == HdrFormat::ScRgb ? "scRGB" :
      hdr.resolved_format == HdrFormat::Hdr10 ? "HDR10" : "SDR";
  const char* source_name =
      hdr.metering_source == 0 ? "Full frame" :
      hdr.metering_source == 1 ? "Filtered population" :
      hdr.metering_source == 2 ? "Game exposure (NGX)" : "Unknown";

  ImGui::Text("Format: %s - resolved=%s", format_name,
              hdr.ready ? "yes" : "no");
  if (hdr.enabled && !hdr.ready) {
    ImGui::TextDisabled("  Warming up - no exposure measurement yet.");
  }
  ImGui::Text("Exposure: %.4f", hdr.exposure);
  ImGui::Text("Scene white: %.4f", hdr.scene_white);
  if (hdr.exposure_reference_luma > 0.0f) {
    ImGui::Text("  reference luma: %.4f",
                hdr.exposure_reference_luma);
  }
  {
    const float diff = hdr.adaptive_scene_white - hdr.scene_white;
    if (hdr.adaptive_scene_white > 0.0f &&
        (diff > 1e-4f || diff < -1e-4f)) {
      ImGui::Text("  adaptive scene white: %.4f",
                  hdr.adaptive_scene_white);
    }
  }
  ImGui::Text("Samples: %u - frames since measure: %u",
              hdr.samples, hdr.frames_since_measure);
  ImGui::Text("Metering source: %s", source_name);
  ImGui::Text("DLSS guides: %s",
              snap.guides_valid ? "valid" : "not seen");
  if (snap.post_dlss_blocked) {
    ImGui::Text("Post-DLSS blocked: %s", snap.post_dlss_block_reason);
  }

  ImGui::Separator();
  ImGui::Text("Game exposure: %s",
              hdr.game_exposure_available ? "available" : "unavailable");
  ImGui::Text("  value: %.4f", hdr.game_exposure);
  ImGui::Text("  texture: %ux%u fmt=%d",
              hdr.game_exposure_width, hdr.game_exposure_height,
              hdr.game_exposure_format);

  ImGui::Separator();
  ImGui::Text("Gain map: %s", hdr.gain_map_ready ? "ready" : "pending");
  ImGui::Text("  range (log2): %.3f .. %.3f",
              hdr.gain_min_log2, hdr.gain_max_log2);
  ImGui::Text("  SDR excursions: %u", hdr.gain_sdr_excursion_pixels);
  ImGui::Text("  out of range: %u", hdr.gain_out_of_range_pixels);

  ImGui::Separator();
  if (!hdr.has_histogram) {
    ImGui::TextDisabled("Histogram: not ready");
  } else {
    ImGui::TextUnformatted("Histogram (log2 luminance)");
    float bins_float[256];
    float peak = 1.0f;
    for (int i = 0; i < 256; ++i) {
      bins_float[i] = static_cast<float>(hdr.histogram[i]);
      if (bins_float[i] > peak) peak = bins_float[i];
    }
    ImGui::PlotHistogram("##hdr_histogram", bins_float, 256, 0, nullptr,
                         0.0f, peak, ImVec2(-1.0f, 80.0f));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Histogram of measured log2 luminance. The population depends on "
          "the active exposure filters.");
    }
  }

  ImGui::Separator();
  ImGui::TextDisabled(
      "Values update asynchronously. Exposure and histogram reflect the "
      "most recent measurement the GPU has finished writing back.");

  return false;
}

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

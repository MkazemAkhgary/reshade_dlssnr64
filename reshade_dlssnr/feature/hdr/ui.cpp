// HDR non-row UI: the exposure-layer tab bar and its bodies.
//
// The three layers are separate sections, so each tab body is just
// DrawSection on that section. That is the whole reason the layers are
// sections and not categories: a tab body has to be able to draw a block of
// settings on its own, and only a section can.
//
// The layer bodies draw with `draw_headers = false` -- the tab label is the
// header -- and the caller already indented, so the drawer must not.

#include "feature/hdr/ui.h"

#include <imgui.h>

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

// Draw one layer section inside its tab, without a header, for the same
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
  };
  const Layer layers[] = {
      {"Game Exposure",
       "Configure the game's exposure as the base layer.",
       hdr::kGameSection,
       feature::ui_state::kHdrGameOpen},
      {"Adaptive Exposure",
       "Histogram-based global exposure or a bounded correction over Game "
       "Exposure.",
       hdr::kAdaptiveSection,
       feature::ui_state::kHdrAdaptiveOpen},
      {"Local Exposure",
       "Spatially varying exposure correction applied after the global "
       "layers.",
       hdr::kLocalSection,
       feature::ui_state::kHdrLocalOpen},
  };
  constexpr int kLayerCount = 3;

  for (int i = 0; i < kLayerCount; ++i) {
    bool layer_changed = false;
    const bool open = PersistentSettingHeader(
        layers[i].label, layers[i].open_key, layer_changed,
        layers[i].tooltip);
    local_changed |= layer_changed;
    if (open) {
      IndentScope body_indent;
      local_changed |= DrawLayerBody(store, options, layers[i].section, key);
    }
  }

  ImGui::TextDisabled(
      "Stack order: Game Exposure base, Adaptive correction, then Local "
      "Exposure. Each layer's filters are independent.");

  changed |= local_changed;
  return local_changed;
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

  if (!PersistentSettingHeader(
          "HDR Diagnostics", feature::ui_state::kHdrDiagnosticsOpen, changed,
          "Live HDR measurement state. Read-only; reflects the current "
          "exposure, histogram, and gain-map values the pipeline is using.")) {
    return false;
  }

  IndentScope indent;
  const StatusSnapshot snap = AddonController::Get().GetStatus();
  const HdrSnapshot hdr = AddonController::Get().GetHdrSnapshot();

  const char* format_name =
      hdr.resolved_format == HdrFormat::ScRgb ? "scRGB" :
      hdr.resolved_format == HdrFormat::Hdr10 ? "HDR10" : "SDR";
  const char* source_name =
      hdr.metering_source == 0 ? "Full frame" :
      hdr.metering_source == 1 ? "Filtered population" :
      hdr.metering_source == 2 ? "Game exposure buffer" : "Unknown";

  ImGui::Text("Format: %s - resolved=%s", format_name,
              hdr.ready ? "yes" : "no");
  ImGui::Text("Exposure: %.4f (target %.4f)",
              hdr.exposure, hdr.target_exposure);
  ImGui::Text("Scene white: %.4f - reference luma: %.4f",
              hdr.scene_white, hdr.exposure_reference_luma);
  ImGui::Text("P_hi luma: %.4f", hdr.p_hi_luma);
  ImGui::Text("Samples: %u - frames since measure: %u",
              hdr.samples, hdr.frames_since_measure);
  ImGui::Text("Measurement generation: %llu",
              static_cast<unsigned long long>(hdr.measurement_generation));
  ImGui::Text("Metering source: %s", source_name);

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

  (void)snap;
  return false;
}

}  // namespace hdr
}  // namespace feature
}  // namespace reshade_dlssnr

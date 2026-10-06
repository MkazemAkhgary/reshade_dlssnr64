#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>

#include "addon_controller.h"
#include "hdr/hdr_state.h"
#include "state/settings.h"
#include "hdr/settings.h"
#include "ui/ui_sections.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

namespace {

const char* const kFormatNames[] = {"Auto", "SDR", "scRGB", "HDR10"};
const char* const kFormatTips[] = {
    "Detect the format automatically from the DLSS output.",
    "Treat the source as SDR sRGB.",
    "Treat the source as linear scRGB (1.0 = 80 nits).",
    "Treat the source as HDR10 PQ (ST.2084)."};

void DrawStatus(const HdrSnapshot& snap, bool hdr_relevant) {
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
    Tip("Format, current exposure, and scene white point.");
  }
  ImGui::SameLine(kControlLabelWidth);

  if (!hdr_relevant) {
    ImGui::TextDisabled("Inactive (no DLSS Output pass)");
  } else if (!snap.ready) {
    ImGui::TextDisabled("Waiting for first measurement...");
  } else {
    ImGui::Text("Format: %s   Exposure: %.3f   White: %.3f",
                format_name, snap.exposure, snap.scene_white);
  }
}

void DrawControls(HdrSettings& hdr, bool& changed) {
  int fmt = static_cast<int>(hdr.format);
  changed |= DrawGroupRow(
      "hdr_format", "Format",
      "How the source image is encoded. Auto detects from the DLSS "
      "output's DXGI format.",
      kFormatNames, kFormatTips, 4, &fmt, nullptr);
  hdr.format = static_cast<HdrFormat>(fmt);

  changed |= DrawIntRow(
      "hdr_measure_interval", "Measure Interval",
      "How often the scene's brightness is measured, in frames.",
      &hdr.measure_interval, 32, 1, 4);

  changed |= DrawFloatRow(
      0, "hdr_midtone", "Midtone Target",
      "Where the scene's median brightness is mapped before the model "
      "sees it. Higher values brighten the input.",
      &hdr.midtone, nullptr, 0.0f, 0.01f, 0.50f, 0.18f);

  changed |= DrawFloatRow(
      0, "hdr_white_percentile", "White Percentile",
      "Which percentile defines the white point.",
      &hdr.white_percentile, nullptr, 0.0f, 0.90f, 0.999f, 0.99f);

  changed |= DrawFloatRow(
      0, "hdr_highlight_headroom", "Highlight Headroom",
      "Scales the effective HDR peak used by BT.2446C. Higher values preserve more SDR headroom so the model can add highlights without consuming the gain map.",
      &hdr.highlight_headroom, nullptr, 0.0f, 0.25f, 16.0f, 8.0f);

  changed |= DrawFloatRow(
      0, "hdr_bt2446_k1", "BT.2446C k1",
      "Linear-region slope. Default matches zentone's skin-tone-tuned Method C.",
      &hdr.bt2446_k1, nullptr, 0.001f, 0.001f, 2.0f, 0.83802f);

  changed |= DrawFloatRow(
      0, "hdr_bt2446_k2", "BT.2446C k2",
      "Log-region slope. Default matches the gain-map reference implementation.",
      &hdr.bt2446_k2, nullptr, 0.001f, 0.001f, 32.0f, 15.09968f);

  changed |= DrawFloatRow(
      0, "hdr_bt2446_k3", "BT.2446C k3",
      "Reference construction parameter retained for parity/trace. The direct Method-C formula uses k1, k2 and k4.",
      &hdr.bt2446_k3, nullptr, 0.001f, 0.001f, 1.0f, 0.74204f);

  changed |= DrawFloatRow(
      0, "hdr_bt2446_k4", "BT.2446C k4",
      "Log-region intercept in nits.",
      &hdr.bt2446_k4, nullptr, 0.001f, 0.001f, 200.0f, 78.99439f);

  changed |= DrawFloatRow(
      0, "hdr_bt2446_crosstalk", "Crosstalk",
      "BT.2446C pre-desaturation amount. Higher values protect highlight hue at the cost of highlight saturation.",
      &hdr.bt2446_crosstalk, nullptr, 0.0f, 0.0f, 0.33f, 0.04f);


  changed |= DrawFloatRow(
      0, "hdr_gain_base_offset", "Gain Base Offset",
      "ISO 21496-1 base offset. Stored directly in the linear SDR gain-map math.",
      &hdr.gain_base_offset, nullptr, 0.0f, 0.0f, 1.0f, 1.0f / 64.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_alternate_offset", "Gain Alternate Offset",
      "ISO 21496-1 alternate offset. Stored directly in the linear HDR gain-map math.",
      &hdr.gain_alternate_offset, nullptr, 0.0f, 0.0f, 1.0f, 1.0f / 64.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_min_log2", "Gain Min",
      "Configured ISO 21496-1 log2 gain metadata minimum. Raw f32 gain remains unquantized.",
      &hdr.gain_min_log2, nullptr, 0.0f, -32.0f, 0.0f, -4.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_max_log2", "Gain Max",
      "Configured ISO 21496-1 log2 gain metadata maximum. Raw f32 gain remains unquantized.",
      &hdr.gain_max_log2, nullptr, 0.0f, 0.0f, 32.0f, 6.0f);

  changed |= DrawFloatRow(
      0, "hdr_rate_cap", "Response Cap",
      "Maximum log2 change in exposure per measurement. 0 disables the "
      "cap. Warmup is exempt either way.",
      &hdr.rate_cap, nullptr, 0.0f, 0.0f, 2.0f, 0.1f);

  changed |= DrawFloatRow(
      0, "hdr_skin_fraction_threshold", "Skin Fraction",
      "Fraction of the frame that must be skin before the skin "
      "histogram is considered.",
      &hdr.skin_fraction_threshold, nullptr, 0.0f, 0.0f, 0.5f, 0.01f);

  changed |= DrawFloatRow(
      0, "hdr_skin_priority", "Skin Priority",
      "How much the skin median influences the target once the gate "
      "above has passed. 0 = scene alone, 1 = skin alone.",
      &hdr.skin_priority, nullptr, 0.7f, 0.0f, 1.0f, 0.7f);

  changed |= DrawFloatRow(
      0, "hdr_depth_fallback_fraction", "Depth Fallback",
      "If the depth filter keeps less than this, the exposure falls "
      "back to the full frame. 1.0 disables the depth filter entirely.",
      &hdr.depth_fallback_fraction, nullptr, 0.0f, 0.0f, 1.0f, 0.20f);
}

void DrawDiagnostics(const HdrSnapshot& snap, const HdrSettings& hdr,
                     bool& diagnostics_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Diagnostics", diagnostics_open, changed)) {
    return;
  }

  if (!snap.has_histogram) {
    ImGui::TextDisabled("Waiting for first histogram measurement...");
  }

  if (snap.has_histogram && ImGui::BeginTable("hdr_diag", 2, ImGuiTableFlags_SizingStretchSame)) {
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
                hdr.white_percentile * 100.0f, snap.p_hi_luma);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Target exposure: %.3f", snap.target_exposure);
    ImGui::TableNextColumn();
    ImGui::Text("Current: %.3f", snap.exposure);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("White (post-headroom): %.3f", CalcEffectiveSceneWhite(snap.scene_white, hdr));

    const char* source_name = "Full frame";
    if (snap.metering_source == 1) source_name = "Depth-filtered";
    else if (snap.metering_source == 2) source_name = "Skin";
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Metering: %s", source_name);
    ImGui::TableNextColumn();
    ImGui::Text("Skin: %.2f%%  Depth kept: %.1f%%",
                snap.skin_fraction * 100.0f,
                snap.depth_filter_survived * 100.0f);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Fallback exposure: %.3f", snap.fallback_exposure);
    ImGui::TableNextColumn();
    ImGui::Text("Fallback white: %.3f", snap.fallback_scene_white);

    ImGui::EndTable();
  }

  if (snap.gain_map_ready) {
    ImGui::Separator();
    ImGui::Text("Gain map range: %.4f .. %.4f log2",
                snap.gain_min_log2, snap.gain_max_log2);
    ImGui::Text("Flagged pixels: %u", snap.gain_clipped_pixels);
  }

  if (!snap.has_histogram) return;

  ImGui::Separator();
  ImGui::TextUnformatted("Histogram (log2 luminance)");

  float bins_float[256];
  float peak = 1.0f;
  for (int i = 0; i < 256; ++i) {
    bins_float[i] = static_cast<float>(snap.histogram[i]);
    if (bins_float[i] > peak) peak = bins_float[i];
  }

  ImGui::PlotHistogram("##hdr_histogram", bins_float, 256,
                       0, nullptr, 0.0f, peak, ImVec2(-1.0f, 80.0f));
}

}  // namespace

void DrawHdrSection(Settings& settings, bool hdr_relevant, bool& changed) {
  if (!PersistentHeader("HDR", settings.ui_hdr_open, changed)) return;

  changed |= ImGui::Checkbox("Enable HDR Handling", &settings.hdr.enable);
  if (ImGui::IsItemHovered()) {
    Tip(
        "Prepares HDR content for the model, which expects a "
        "display-referred SDR image.\n"
        "Only affects passes at DLSS Output.");
  }

  if (!hdr_relevant) ImGui::BeginDisabled();

  const bool hdr_on = settings.hdr.enable;

  if (hdr_on) {
    DrawStatus(AddonController::Get().GetHdrSnapshot(), hdr_relevant);
  }

  WithDisabled(!hdr_on, [&] { DrawControls(settings.hdr, changed); });

  if (hdr_on) {
    DrawDiagnostics(AddonController::Get().GetHdrSnapshot(), settings.hdr,
                    settings.ui_hdr_diagnostics_open, changed);
  }

  if (!hdr_relevant) ImGui::EndDisabled();
}

}  // namespace reshade_dlssnr

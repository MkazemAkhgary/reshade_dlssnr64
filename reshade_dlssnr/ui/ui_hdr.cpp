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

  static const char* const kExposureMethodLabels[] = {
      "Median (Legacy)", "Auto Exposure + Local", "Game Exposure Buffer"};
  static const char* const kExposureMethodTips[] = {
      "Legacy median exposure. When structure detection has a valid persistent mask, the masked population can bias metering.",
      "Histogram eye adaptation with separate brighten/darken speeds, with optional structure-mask priority, followed by the base/detail local exposure stage.",
      "Use the 1x1 ExposureTexture supplied by the game's DLSS/NGX call. Falls back to the selected histogram method when the game does not provide a usable buffer."};
  int exposure_method = static_cast<int>(hdr.exposure_method);
  changed |= DrawComboRow(
      "hdr_exposure_method", "Exposure Method",
      "Choose how the scene brightness target is measured.",
      &exposure_method, kExposureMethodLabels, kExposureMethodTips, 3);
  hdr.exposure_method = static_cast<HdrExposureMethod>(exposure_method);

  changed |= DrawFloatRow(
      0, "hdr_midtone", "Midtone Target",
      "Neutral model luminance target. 0.18 is the normal reference; changing it is an explicit artistic bias, not a required per-scene adjustment.",
      &hdr.midtone, nullptr, 0.0f, 0.01f, 0.50f, 0.18f);

  if (hdr.exposure_method == HdrExposureMethod::Median) {
    changed |= DrawFloatRow(
        0, "hdr_white_percentile", "White Percentile",
        "Which percentile defines the white point for the fixed exposure method.",
        &hdr.white_percentile, nullptr, 0.0f, 0.90f, 0.999f, 0.99f);
  }

  changed |= DrawFloatRow(
      0, "hdr_highlight_headroom", "Highlight Headroom",
      "Scales the effective HDR peak used by BT.2446C. Higher values preserve more SDR headroom so the model can add highlights without consuming the gain map.",
      &hdr.highlight_headroom, nullptr, 0.0f, 0.25f, 16.0f, 8.0f);

  if (hdr.exposure_method == HdrExposureMethod::AutoLocal) {
    changed |= DrawFloatRow(
        0, "hdr_auto_low_percent", "Low Percent",
        "Lower percentile of the histogram band used for global eye adaptation.",
        &hdr.auto_low_percent, nullptr, 0.0f, 0.0f, 0.99f, 0.70f);

    changed |= DrawFloatRow(
        0, "hdr_auto_high_percent", "High Percent",
        "Upper percentile of the histogram band used for global eye adaptation.",
        &hdr.auto_high_percent, nullptr, 0.0f, 0.01f, 0.999f, 0.90f);

    changed |= DrawFloatRow(
        0, "hdr_speed_up", "Speed Up",
        "Maximum exposure increase in stops per second when adapting brighter.",
        &hdr.speed_up, nullptr, 0.0f, 0.0f, 20.0f, 3.0f);

    changed |= DrawFloatRow(
        0, "hdr_speed_down", "Speed Down",
        "Maximum exposure decrease in stops per second when adapting darker.",
        &hdr.speed_down, nullptr, 0.0f, 0.0f, 20.0f, 1.0f);

    changed |= DrawFloatRow(
        0, "hdr_local_exposure_strength", "Local Exposure",
        "How strongly local luminance adjusts the model-facing exposure. 0 disables the local stage.",
        &hdr.local_exposure_strength, nullptr, 0.0f, 0.0f, 1.0f, 0.50f);

    changed |= DrawFloatRow(
        0, "hdr_local_max_brighten", "Local Max Brighten",
        "Maximum local brightening in stops for dark regions.",
        &hdr.local_max_brighten, nullptr, 0.0f, 0.0f, 4.0f, 1.75f);

    changed |= DrawFloatRow(
        0, "hdr_local_max_darken", "Local Max Darken",
        "Maximum local darkening in stops for bright regions.",
        &hdr.local_max_darken, nullptr, 0.0f, 0.0f, 4.0f, 1.75f);
  }

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
      "ISO 21496-1 base offset. Applied to SDR luminance for the single-channel gain map; it is not added independently to RGB channels.",
      &hdr.gain_base_offset, nullptr, 0.0f, 0.0f, 1.0f, 1.0f / 64.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_alternate_offset", "Gain Alternate Offset",
      "ISO 21496-1 alternate offset. Applied to HDR luminance for the single-channel gain map; it is not added independently to RGB channels.",
      &hdr.gain_alternate_offset, nullptr, 0.0f, 0.0f, 1.0f, 1.0f / 64.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_min_log2", "Gain Min",
      "Configured ISO 21496-1 log2 gain metadata minimum. Raw f32 gain remains unquantized.",
      &hdr.gain_min_log2, nullptr, 0.0f, -32.0f, 0.0f, -4.0f);

  changed |= DrawFloatRow(
      0, "hdr_gain_max_log2", "Gain Max",
      "Configured ISO 21496-1 log2 gain metadata maximum. Raw f32 gain remains unquantized.",
      &hdr.gain_max_log2, nullptr, 0.0f, 0.0f, 32.0f, 6.0f);

  if (hdr.exposure_method == HdrExposureMethod::Median) {
    changed |= DrawFloatRow(
        0, "hdr_rate_cap", "Response Cap",
        "Maximum log2 change in exposure per measurement for the legacy median method.",
        &hdr.rate_cap, nullptr, 0.0f, 0.0f, 2.0f, 0.1f);
  }
}

void DrawDiagnostics(const HdrSnapshot& snap, const HdrSettings& hdr,
                     bool& diagnostics_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Diagnostics", diagnostics_open, changed)) {
    return;
  }

  const bool game_mode = hdr.exposure_method == HdrExposureMethod::GameExposureBuffer;
  const bool show_table = snap.has_histogram || game_mode;
  if (!show_table) {
    ImGui::TextDisabled("Waiting for first measurement...");
  }

  if (show_table && ImGui::BeginTable("hdr_diag", 2, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    if (snap.has_histogram) {
      ImGui::Text("Samples: %u", snap.samples);
    } else {
      ImGui::TextUnformatted("Samples: n/a (game buffer)");
    }
    ImGui::TableNextColumn();
    ImGui::Text("Frames since: %u", snap.frames_since_measure);

    const char* exposure_method_name =
        hdr.exposure_method == HdrExposureMethod::AutoLocal
            ? "Auto Exposure + Local"
            : (game_mode ? "Game Exposure Buffer"
                         : "Median (Legacy)");

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Exposure reference (100-nit domain): %.4f",
                snap.exposure_reference_luma);
    ImGui::TableNextColumn();
    if (snap.has_histogram) {
      ImGui::Text("White source (same metering set): %.4f", snap.p_hi_luma);
    } else {
      ImGui::TextUnformatted("White source: fixed 1.0 (game buffer)");
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Exposure method: %s", exposure_method_name);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(
        snap.metering_source == static_cast<int>(MeteringSource::GameExposureBuffer)
            ? "Game Exposure Buffer"
            : (snap.metering_source == static_cast<int>(MeteringSource::SkinMaskPriority)
                   ? "Skin Mask Priority"
                   : "Full Frame"));

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Target exposure: %.3f", snap.target_exposure);
    ImGui::TableNextColumn();
    ImGui::Text("Current: %.3f", snap.exposure);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("White (post-headroom, 100-nit domain): %.3f",
                CalcEffectiveSceneWhite(snap.scene_white, hdr));
    ImGui::TableNextColumn();
    ImGui::Text("Metering: %s",
                game_mode ? "Game ExposureTexture (histogram fallback)"
                          : "Full frame histogram");

    if (game_mode) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Game buffer: %s",
                  snap.game_exposure_available ? "FOUND (last valid)" : "NOT FOUND");
      ImGui::TableNextColumn();
      if (snap.game_exposure_available) {
        ImGui::Text("Exposure: %.5f (%ux%u, fmt %d)", snap.game_exposure,
                    snap.game_exposure_width, snap.game_exposure_height,
                    snap.game_exposure_format);
      } else {
        ImGui::TextUnformatted("Waiting for usable ExposureTexture");
      }
    }

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Local exposure: %s",
                hdr.exposure_method == HdrExposureMethod::AutoLocal
                    ? "base/detail" : "off");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(game_mode
                               ? "Source: NGX ExposureTexture"
                               : "Source: histogram");

    ImGui::EndTable();
  }

  if (snap.gain_map_ready) {
    ImGui::Separator();
    ImGui::Text("Gain map range: %.4f .. %.4f log2",
                snap.gain_min_log2, snap.gain_max_log2);
    ImGui::Text("SDR excursions: %u  |  Gain outside range: %u",
                snap.gain_sdr_clipped_pixels,
                snap.gain_out_of_range_pixels);
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

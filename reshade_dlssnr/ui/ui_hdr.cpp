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
    "Detect the format automatically from the active Pre/Post-DLSS source.",
    "Treat the source as SDR sRGB.",
    "Treat the source as linear scRGB (1.0 = 80 nits).",
    "Treat the source as HDR10 PQ (ST.2084)."};

void DrawStatus(const HdrSnapshot& snap, bool hdr_relevant) {
  const char* format_name = "SDR";
  switch (snap.resolved_format) {
    case HdrFormat::Auto: format_name = "Auto"; break;
    case HdrFormat::Sdr: format_name = "SDR"; break;
    case HdrFormat::ScRgb: format_name = "scRGB"; break;
    case HdrFormat::Hdr10: format_name = "HDR10"; break;
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Status");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    Tip("Format, resolved exposure, and scene-white point.");
  ImGui::SameLine(kControlLabelWidth);

  if (!hdr_relevant) {
    ImGui::TextDisabled("Inactive (no Pre/Post-DLSS pass)");
  } else if (!snap.ready) {
    ImGui::TextDisabled("Waiting for first measurement...");
  } else {
    ImGui::Text("Format: %s   Exposure: %.3f   White: %.3f",
                format_name, snap.exposure, snap.scene_white);
  }
}

void DrawFilterControls(const char* id, ExposureFilterSettings& filters,
                        bool& changed, const char* hint = nullptr) {
  ImGui::PushID(id);
  ImGui::Separator();
  ImGui::TextUnformatted("Filters");
  ImGui::SameLine();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip(hint != nullptr ? hint :
        "Enabled filters are combined. Each layer owns its own filter set. "
        "When a mask or depth resource is unavailable, that filter is skipped.");
  }

  changed |= ImGui::Checkbox("Structure Mask", &filters.structure_mask);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    Tip("Meter or correct only pixels weighted by the persistent Structure Detection mask.");
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Depth", &filters.depth);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    Tip("Restrict this layer to the selected normalized depth interval.");
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Percentile Range", &filters.percentile_range);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    Tip("For Game/Adaptive, select a percentile band of this layer's histogram. "
        "For Local Exposure, fade the local correction outside its measured luminance band.");

  if (filters.depth) {
    changed |= DrawFloatRow(0, "depth_near", "Depth Near",
                            "Near end of the accepted normalized depth interval.",
                            &filters.depth_near, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);
    changed |= DrawFloatRow(0, "depth_far", "Depth Far",
                            "Far end of the accepted normalized depth interval.",
                            &filters.depth_far, nullptr, 1.0f, 0.0f, 1.0f, 1.0f);
    if (filters.depth_near > filters.depth_far) {
      std::swap(filters.depth_near, filters.depth_far);
      changed = true;
    }
  }
  if (filters.percentile_range) {
    changed |= DrawFloatRow(0, "percentile_low", "Low Percentile",
                            "Lower percentile boundary. Game/Adaptive use the band for metering; Local uses it as a luminance protection band.",
                            &filters.percentile_low, nullptr, 0.70f, 0.0f, 0.99f, 0.70f);
    changed |= DrawFloatRow(0, "percentile_high", "High Percentile",
                            "Upper percentile boundary for this layer.",
                            &filters.percentile_high, nullptr, 0.90f, 0.01f, 1.0f, 0.90f);
    if (filters.percentile_high <= filters.percentile_low) {
      filters.percentile_high = std::min(1.0f, filters.percentile_low + 0.01f);
      if (filters.percentile_high <= filters.percentile_low)
        filters.percentile_low = std::max(0.0f, filters.percentile_high - 0.01f);
      changed = true;
    }
  }
  ImGui::PopID();
}

void DrawGameLayer(GameExposureLayerSettings& layer, bool& changed) {
  changed |= ImGui::Checkbox("Enabled##game_layer", &layer.enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Use the game's NGX ExposureTexture as the base exposure for the stack.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Base exposure from the game's NGX ExposureTexture");
  WithDisabled(!layer.enabled, [&] {
    changed |= DrawFloatRow(0, "game_bias_stops", "Exposure Bias",
                            "Manual bias relative to the game's exposure, in stops. Zero leaves the game's value untouched.",
                            &layer.bias_stops, nullptr, 0.0f, -4.0f, 4.0f, 0.0f);
    changed |= ImGui::Checkbox("Bounded Adaptive Tuning", &layer.bounded_tuning);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      Tip("Allow the adaptive measurement to suggest a small correction, clipped to Max Tuning Stops. The game remains the global exposure anchor.");
    if (layer.bounded_tuning) {
      changed |= DrawFloatRow(0, "game_tuning_strength", "Tuning Strength",
                              "Blend toward the adaptive exposure suggestion.",
                              &layer.tuning_strength, nullptr, 0.25f, 0.0f, 1.0f, 0.25f);
      changed |= DrawFloatRow(0, "game_tuning_stops", "Max Tuning Stops",
                              "Maximum absolute correction away from the game's value.",
                              &layer.max_tuning_stops, nullptr, 0.5f, 0.0f, 2.0f, 0.5f);
      changed |= DrawFloatRow(0, "game_tuning_midtone", "Tuning Midtone",
                              "Target luminance used only to estimate the bounded correction.",
                              &layer.tuning_midtone, nullptr, 0.18f, 0.01f, 0.5f, 0.18f);
      changed |= DrawFloatRow(0, "game_tuning_speed_up", "Brighten Speed",
                              "Maximum brighten correction change in stops per second.",
                              &layer.tuning_speed_up, nullptr, 3.0f, 0.0f, 20.0f, 3.0f);
      changed |= DrawFloatRow(0, "game_tuning_speed_down", "Darken Speed",
                              "Maximum darken correction change in stops per second.",
                              &layer.tuning_speed_down, nullptr, 1.0f, 0.0f, 20.0f, 1.0f);
    }
    ImGui::TextDisabled("These filters scope Bounded Adaptive Tuning only. The game value is a global scalar.");
    WithDisabled(!layer.bounded_tuning, [&] {
      DrawFilterControls(
          "game_filters", layer.filters, changed,
          "These filters select pixels for the optional bounded tuning estimate. "
          "NGX supplies one global exposure scalar, so filters cannot spatially edit it.");
    });
  });
}

void DrawAdaptiveLayer(AdaptiveExposureLayerSettings& layer, bool& changed) {
  changed |= ImGui::Checkbox("Enabled##adaptive_layer", &layer.enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Use histogram-derived global exposure or a bounded correction over Game Exposure.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Histogram-driven exposure");
  WithDisabled(!layer.enabled, [&] {
    changed |= DrawFloatRow(0, "adaptive_midtone", "Midtone Target",
                            "Target luminance for the selected population.",
                            &layer.midtone, nullptr, 0.18f, 0.01f, 0.5f, 0.18f);
    changed |= DrawFloatRow(0, "adaptive_speed_up", "Brighten Speed",
                            "Maximum exposure increase in stops per second.",
                            &layer.speed_up, nullptr, 3.0f, 0.0f, 20.0f, 3.0f);
    changed |= DrawFloatRow(0, "adaptive_speed_down", "Darken Speed",
                            "Maximum exposure decrease in stops per second.",
                            &layer.speed_down, nullptr, 1.0f, 0.0f, 20.0f, 1.0f);
    changed |= DrawFloatRow(0, "adaptive_strength_over_game", "Strength Over Game",
                            "When Game Exposure is active, add this fraction of the adaptive target's stop correction. With Game disabled, Adaptive is the base exposure.",
                            &layer.strength_over_game, nullptr, 0.0f, 0.0f, 1.0f, 0.0f);
    changed |= DrawFloatRow(0, "adaptive_max_correction", "Max Correction Stops",
                            "Maximum adjustment around Game Exposure when both layers are enabled.",
                            &layer.max_correction_stops, nullptr, 0.75f, 0.0f, 3.0f, 0.75f);
    DrawFilterControls("adaptive_filters", layer.filters, changed);
  });
}

void DrawLocalLayer(LocalExposureLayerSettings& layer, bool& changed) {
  changed |= ImGui::Checkbox("Enabled##local_layer", &layer.enabled);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Apply spatially varying exposure correction after the global exposure layers.");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Spatial base/detail correction");
  WithDisabled(!layer.enabled, [&] {
    changed |= DrawFloatRow(0, "local_strength", "Strength",
                            "Blend factor for the spatial exposure adjustment.",
                            &layer.strength, nullptr, 0.5f, 0.0f, 1.0f, 0.5f);
    changed |= DrawFloatRow(0, "local_max_brighten", "Max Brighten",
                            "Maximum local brightening in stops.",
                            &layer.max_brighten, nullptr, 1.75f, 0.0f, 4.0f, 1.75f);
    changed |= DrawFloatRow(0, "local_max_darken", "Max Darken",
                            "Maximum local darkening in stops.",
                            &layer.max_darken, nullptr, 1.75f, 0.0f, 4.0f, 1.75f);
    changed |= DrawFloatRow(0, "local_midtone", "Local Midtone",
                            "Reference luminance for the local base/detail operator.",
                            &layer.midtone, nullptr, 0.18f, 0.01f, 0.5f, 0.18f);
    changed |= DrawFloatRow(0, "local_block_size", "Neighborhood Block Size (px)",
                            "Neighborhood diameter (4-512 px). Larger sizes spread the local-base taps farther apart. Effective diameter is capped at half the shorter source dimension; the field stays continuous per pixel to avoid visible block boundaries.",
                            &layer.block_size_px, nullptr, 24.0f, 4.0f, 512.0f, 24.0f);
    changed |= DrawIntRow("local_sample_count", "Neighborhood Samples",
                          "Number of neighborhood taps (4-48). Higher counts fill gaps in the same neighborhood and cost more GPU time; this does not increase the block radius.",
                          &layer.sample_count, 4, 48, 24);
    changed |= DrawFloatRow(0, "local_edge_sharpness", "Luma Edge Protection",
                            "Controls how strongly samples with different log-luminance are rejected. Higher values reduce correction bleed across facial features and silhouettes; 0 disables luminance-edge weighting. This is not a semantic face detector.",
                            &layer.edge_sharpness, nullptr, 4.0f, 0.0f, 8.0f, 4.0f);
    changed |= ImGui::Checkbox("Depth Continuity##local_depth_continuity", &layer.depth_continuity);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      Tip("When a DLSS depth guide is available, exclude neighborhood samples across depth discontinuities. This is separate from the Depth filter, which restricts which pixels may receive the correction.");
    if (layer.depth_continuity) {
      changed |= DrawFloatRow(0, "local_depth_edge_threshold", "Depth Edge Threshold",
                              "Maximum normalized depth difference allowed between neighborhood samples. Lower values separate foreground/background more aggressively; ignored when depth is unavailable.",
                              &layer.depth_edge_threshold, nullptr, 0.01f, 0.001f, 0.1f, 0.01f);
    }
    DrawFilterControls("local_filters", layer.filters, changed);
  });
}

void DrawControls(HdrSettings& hdr, bool& changed) {
  int fmt = static_cast<int>(hdr.format);
  changed |= DrawGroupRow(
      "hdr_format", "Format",
      "How the source image is encoded. Auto detects from the DLSS output's DXGI format.",
      kFormatNames, kFormatTips, 4, &fmt, nullptr);
  hdr.format = static_cast<HdrFormat>(fmt);

  changed |= DrawIntRow("hdr_measure_interval", "Measure Interval",
                        "How often exposure histograms are measured, in frames.",
                        &hdr.measure_interval, 1, 32, 4);
  changed |= DrawFloatRow(0, "hdr_highlight_headroom", "Highlight Headroom",
                          "Scales the effective HDR peak used by BT.2446C. This does not change how exposure is interpreted.",
                          &hdr.highlight_headroom, nullptr, 8.0f, 0.25f, 16.0f, 8.0f);

  if (ImGui::BeginTabBar("HdrExposureLayers")) {
    const bool game_open = ImGui::BeginTabItem("Game Exposure");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Configure the game's exposure as the base layer.");
    }
    if (game_open) {
      DrawGameLayer(hdr.game_exposure, changed);
      ImGui::EndTabItem();
    }

    const bool adaptive_open = ImGui::BeginTabItem("Adaptive Exposure");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Configure histogram-based global exposure or a correction over Game Exposure.");
    }
    if (adaptive_open) {
      DrawAdaptiveLayer(hdr.adaptive_exposure, changed);
      ImGui::EndTabItem();
    }

    const bool local_open = ImGui::BeginTabItem("Local Exposure");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Configure local spatial exposure correction and its independent filters.");
    }
    if (local_open) {
      DrawLocalLayer(hdr.local_exposure, changed);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::TextDisabled("Stack order: Game Exposure base, Adaptive correction, then Local Exposure. Each layer's filters are independent.");

  changed |= DrawFloatRow(0, "hdr_bt2446_k1", "BT.2446C k1",
                          "Linear-region slope.", &hdr.bt2446_k1, nullptr,
                          0.83802f, 0.001f, 2.0f, 0.83802f);
  changed |= DrawFloatRow(0, "hdr_bt2446_k2", "BT.2446C k2",
                          "Log-region slope.", &hdr.bt2446_k2, nullptr,
                          15.09968f, 0.001f, 32.0f, 15.09968f);
  changed |= DrawFloatRow(0, "hdr_bt2446_k3", "BT.2446C k3",
                          "Reference construction parameter retained for parity/trace.",
                          &hdr.bt2446_k3, nullptr, 0.74204f, 0.001f, 1.0f, 0.74204f);
  changed |= DrawFloatRow(0, "hdr_bt2446_k4", "BT.2446C k4",
                          "Log-region intercept in nits.", &hdr.bt2446_k4, nullptr,
                          78.99439f, 0.001f, 200.0f, 78.99439f);
  changed |= DrawFloatRow(0, "hdr_bt2446_crosstalk", "Crosstalk",
                          "BT.2446C pre-desaturation amount.", &hdr.bt2446_crosstalk,
                          nullptr, 0.04f, 0.0f, 0.33f, 0.04f);
  changed |= DrawFloatRow(0, "hdr_gain_min_log2", "Gain Min",
                          "Configured log2 gain metadata minimum.",
                          &hdr.gain_min_log2, nullptr, -4.0f, -32.0f, 0.0f, -4.0f);
  changed |= DrawFloatRow(0, "hdr_gain_max_log2", "Gain Max",
                          "Configured log2 gain metadata maximum.",
                          &hdr.gain_max_log2, nullptr, 6.0f, 0.0f, 32.0f, 6.0f);
}

void DrawDiagnostics(const HdrSnapshot& snap, const HdrSettings& hdr,
                     bool& diagnostics_open, bool& changed) {
  IndentScope indent;
  if (!PersistentSubHeader("Diagnostics", diagnostics_open, changed)) return;

  const bool game_enabled = hdr.game_exposure.enabled;
  const bool any_layer = game_enabled || hdr.adaptive_exposure.enabled || hdr.local_exposure.enabled;
  if (!snap.has_histogram && !game_enabled) {
    ImGui::TextDisabled("Waiting for first measurement...");
  }
  if (snap.has_histogram || game_enabled) {
    if (ImGui::BeginTable("hdr_diag", 2, ImGuiTableFlags_SizingStretchSame)) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Samples: %s", snap.has_histogram ? "measured" : "n/a");
      ImGui::TableNextColumn();
      ImGui::Text("Frames since: %u", snap.frames_since_measure);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Exposure reference: %.4f", snap.exposure_reference_luma);
      ImGui::TableNextColumn();
      ImGui::Text("Current exposure: %.4f", snap.exposure);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Scene white: %.3f", CalcEffectiveSceneWhite(snap.scene_white, hdr));
      ImGui::TableNextColumn();
      ImGui::Text("White percentile: %.4f", snap.p_hi_luma);

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Stack: %s%s%s",
                  game_enabled ? "Game" : "",
                  hdr.adaptive_exposure.enabled ? (game_enabled ? " + Adaptive" : "Adaptive") : "",
                  hdr.local_exposure.enabled ? ((game_enabled || hdr.adaptive_exposure.enabled) ? " + Local" : "Local") : "");
      ImGui::TableNextColumn();
      ImGui::Text("Metering set: %s",
                  snap.metering_source == static_cast<int>(MeteringSource::GameExposureBuffer)
                      ? "Game exposure" :
                  snap.metering_source == static_cast<int>(MeteringSource::FilteredPopulation)
                      ? "Filtered population" : "Full frame");

      if (game_enabled) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("Game buffer: %s", snap.game_exposure_available ? "FOUND (last valid)" : "NOT FOUND");
        ImGui::TableNextColumn();
        if (snap.game_exposure_available) {
          ImGui::Text("%.5f (%ux%u, fmt %d)", snap.game_exposure,
                      snap.game_exposure_width, snap.game_exposure_height,
                      snap.game_exposure_format);
        } else {
          ImGui::TextUnformatted("Waiting for ExposureTexture");
        }
      }
      ImGui::EndTable();
    }
  }

  ImGui::Text("Local exposure correction: %s", hdr.local_exposure.enabled ? "enabled" : "disabled");
  if (hdr.local_exposure.enabled && hdr.local_exposure.filters.percentile_range) {
    ImGui::Text("Local luminance band: %.5f .. %.5f",
                snap.local_luma_min, snap.local_luma_max);
  }
  if (!any_layer) ImGui::TextDisabled("All exposure layers are disabled.");

  if (snap.gain_map_ready) {
    ImGui::Separator();
    ImGui::Text("Gain map range: %.4f .. %.4f log2", snap.gain_min_log2, snap.gain_max_log2);
    ImGui::Text("SDR excursions: %u  |  Gain outside range: %u",
                snap.gain_sdr_excursion_pixels, snap.gain_out_of_range_pixels);
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
  ImGui::PlotHistogram("##hdr_histogram", bins_float, 256, 0, nullptr,
                       0.0f, peak, ImVec2(-1.0f, 80.0f));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Histogram of measured log2 luminance. The population depends on the active exposure filters.");
  }
}

}  // namespace

void DrawHdrSection(Settings& settings, bool hdr_relevant, bool& changed) {
  if (!PersistentHeader(
          "HDR", settings.ui_hdr_open, changed,
          "Prepare Pre-DLSS and Post-DLSS source images for the model's SDR input domain and configure the exposure layers.")) {
    return;
  }

  IndentScope indent;
  if (!hdr_relevant) ImGui::BeginDisabled();
  changed |= ImGui::Checkbox("Enable HDR Handling", &settings.hdr.enable);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    Tip("Prepares the current Pre-DLSS or Post-DLSS source before the first pass. Backbuffer-only configurations do not use HDR measurement.");
  if (!hdr_relevant) ImGui::EndDisabled();

  const bool hdr_on = settings.hdr.enable;
  if (!hdr_relevant) {
    DrawStatus(AddonController::Get().GetHdrSnapshot(), false);
    ImGui::TextDisabled("Enable a Pre-DLSS or Post-DLSS pass to use HDR handling.");
    return;
  }
  if (hdr_on) DrawStatus(AddonController::Get().GetHdrSnapshot(), true);
  WithDisabled(!hdr_on, [&] { DrawControls(settings.hdr, changed); });
  if (hdr_on) {
    DrawDiagnostics(AddonController::Get().GetHdrSnapshot(), settings.hdr,
                    settings.ui_hdr_diagnostics_open, changed);
  }
}

}  // namespace reshade_dlssnr

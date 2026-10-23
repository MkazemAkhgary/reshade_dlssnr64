#include <imgui.h>
#include <reshade.hpp>

#include "ui/ui_sections.h"
#include "profiling/profile.h"

#include <cstdio>
#include <unordered_map>

#include "addon_controller.h"
#include "feature/settings_store.h"
#include "ctl/values.h"
#include "feature/hdr/view.h"
#include "feature/pass/settings.h"
#include "feature/pass/ui.h"
#include "feature/pass/view.h"
#include "state/settings.h"
#include "status.h"
#include "temporal/stats.h"
#include "ui/ui_widgets.h"
#include "feature/ui_state/settings.h"

namespace reshade_dlssnr {


namespace {

void FormatPassStatus(const PassStatus& pass,
                      InjectionStage stage,
                      float working_scale,
                      bool post_dlss_blocked,
                      char* out, size_t out_size) {
  const char* stage_name =
      stage == InjectionStage::PreDLSS ? "Pre-DLSS" :
      stage == InjectionStage::PostDLSS ? "Post-DLSS" : "Backbuffer";

  char scale[24] = "";
  if (pass.enabled && working_scale < 0.999f) {
    std::snprintf(scale, sizeof(scale), " · %.3gx",
                  static_cast<double>(working_scale));
  }

  if (!pass.enabled) {
    std::snprintf(out, out_size, "Disabled");
    return;
  }
  if (post_dlss_blocked && stage == InjectionStage::PostDLSS) {
    std::snprintf(out, out_size, "%s · blocked%s", stage_name, scale);
    return;
  }
  if (pass.failed) {
    std::snprintf(out, out_size, "%s · FAILED: %s%s",
                  stage_name,
                  pass.reason[0] != '\0' ? pass.reason : "unknown error",
                  scale);
    return;
  }
  if (!pass.has_feature) {
    std::snprintf(out, out_size, "%s · initializing%s", stage_name, scale);
    return;
  }
  std::snprintf(out, out_size, "%s · running%s", stage_name, scale);
}

}  // namespace

void DrawStatusSection(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawStatusSection");
  if (!PersistentSettingHeader(
          "Status", feature::ui_state::kStatusOpen, changed,
          "Live addon, NGX, guide, HDR, and per-pass health. Pass rows use the same target-stage grouping as execution.")) {
    return;
  }

  IndentScope indent;
  const StatusSnapshot snap = AddonController::Get().GetStatus();

  auto bullet = [](bool good) {
    const ImVec4 color = good ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                              : ImVec4(0.9f, 0.4f, 0.4f, 1.0f);

    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.35f,
        ImGui::GetColorU32(color));

    ImGui::SameLine();
  };

  auto cell = [&](bool good, const char* label, const char* value,
                  const char* tooltip) {
    bullet(good);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s: %s", label, value);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("%s", tooltip);
    }
  };

  char dlssnr_buf[16];
  std::snprintf(dlssnr_buf, sizeof(dlssnr_buf), "%s",
                snap.enabled ? "Enabled" : "Off");

  char runtime_buf[kStatusTextLength + 32];
  if (snap.runtime_initialized) {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Initialized");
  } else if (snap.runtime_reason[0] != '\0') {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Not initialized — %s",
                  snap.runtime_reason);
  } else {
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Not initialized");
  }

  char guides_buf[48];
  std::snprintf(guides_buf, sizeof(guides_buf), "%s",
                snap.guides_valid ? "Valid (depth + motion)" : "Not seen");

  // HDR settings are pass-keyed. Status follows the selected pass, matching
  // the exposure snapshot shown by the HDR diagnostics section.
  const ctl::InstanceKey hdr_key = feature::pass::PassKey(settings.active_pass_id);
  const ctl::ValueStore& store = feature::SettingsStore();
  const bool hdr_enabled = feature::hdr::Read(store, hdr_key).enable;
  const feature::pass::PassSnapshot active_pass =
      feature::pass::Read(store, hdr_key);
  const bool hdr_in_use = settings.enabled && active_pass.enabled &&
                          active_pass.target !=
                              static_cast<int>(InjectionStage::Backbuffer);
  const bool hdr_good = !hdr_enabled || !hdr_in_use || snap.hdr_ready;
  char hdr_buf[96];
  if (!hdr_enabled) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Off");
  } else if (!hdr_in_use) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Inactive (no Pre/Post-DLSS pass)");
  } else if (snap.hdr_ready) {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Active · exp %.3f · white %.3f",
                  snap.hdr_exposure, snap.hdr_scene_white);
  } else {
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Warming up");
  }

  if (ImGui::BeginTable("status_shared", 2,
                        ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    cell(snap.enabled, "DLSSNR", dlssnr_buf,
         "Master enable state. When disabled, no pass should submit DLSSNR work.");
    ImGui::TableNextColumn();
    cell(snap.runtime_initialized, "NGX runtime", runtime_buf,
         "Whether the DLSSNR NGX runtime loaded and initialized. Any reason text follows the runtime state.");

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    cell(snap.guides_valid, "DLSS guides", guides_buf,
         "Depth and motion vectors captured from the current DLSS evaluation. Temporal processing needs current-frame guides.");
    ImGui::TableNextColumn();
    cell(hdr_good, "HDR", hdr_buf,
         "HDR handling status. It is relevant only when an enabled pass targets Pre-DLSS or Post-DLSS.");

    ImGui::EndTable();
  }

  if (hdr_enabled && hdr_in_use && snap.gain_map_ready) {
    ImGui::Spacing();
    ImGui::Text("Gain map: %.3f .. %.3f log2 · SDR excursions: %u · gain-range: %u",
                snap.gain_map_min_log2, snap.gain_map_max_log2,
                snap.gain_map_sdr_excursion_pixels,
                snap.gain_map_out_of_range_pixels);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Observed gain-map range and diagnostic counts for SDR-carrier excursions and gains outside the configured range.");
    }
  }

  if (snap.post_dlss_blocked) {
    ImGui::Spacing();
    bullet(false);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(snap.post_dlss_block_reason);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Why Post-DLSS work is currently blocked. Check this message before troubleshooting a Post-DLSS pass.");
    }
  }

  ImGui::Spacing();
  if (ImGui::Button("Restart Addon")) {
    AddonController::Get().RestartAll();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    Tip("Safely restart NGX and addon resources after the current GPU queue work completes.");
  }

  ImGui::Separator();

  // Map stable ids to display positions once. Position is presentation data
  // used only for the "Pass N" label; status and settings are matched by id.
  std::unordered_map<feature::pass::PassId, size_t> position_by_id;
  position_by_id.reserve(settings.pass_order.size());
  for (size_t position = 0; position < settings.pass_order.size(); ++position) {
    position_by_id.emplace(settings.pass_order[position], position);
  }

  // Match the runtime's established execution grouping: Pre-DLSS, then
  // Post-DLSS, then Backbuffer. Within each target group, retain pass/tab order.
  constexpr InjectionStage kStatusStageOrder[] = {
      InjectionStage::PreDLSS,
      InjectionStage::PostDLSS,
      InjectionStage::Backbuffer,
  };
  for (const InjectionStage display_stage : kStatusStageOrder) {
    for (const PassStatusEntry& entry : snap.passes) {
      const auto position_it = position_by_id.find(entry.id);
      if (position_it == position_by_id.end()) continue;
      const size_t position = position_it->second;
      const feature::pass::PassSnapshot pass =
          feature::pass::Read(feature::SettingsStore(), feature::pass::PassKey(entry.id));
      if (pass.target != static_cast<int>(display_stage)) continue;

      char buf[kStatusTextLength];
      // Read the working scale from the store so the row shows the value
      // the runtime actually runs at.
      FormatPassStatus(entry.status, display_stage, pass.working_scale,
                       snap.post_dlss_blocked, buf, sizeof(buf));

      char label[32];
      std::snprintf(label, sizeof(label), "Pass %u",
                    static_cast<unsigned>(position + 1));

      const bool stage_blocked =
          snap.post_dlss_blocked &&
          display_stage == InjectionStage::PostDLSS;
      const bool ok = entry.status.enabled && !entry.status.failed &&
                      !stage_blocked;

      bullet(ok);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Original pass index %u. Display order is grouped Pre-DLSS, Post-DLSS, then Backbuffer while preserving pass order inside each group.",
            static_cast<unsigned>(position + 1));
      }

      ImGui::SameLine(kControlLabelWidth);
      ImGui::TextUnformatted(buf);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Current runtime status for this pass, including its target stage, working scale, initialization state, and any failure reason.");
      }
    }
  }
}

namespace feature {
namespace pass {

// Hook body for the last row of the Temporal Reprojection category. The
// category header owns the collapsible; this only draws the readout.
bool DrawTemporalStats(const void* /*context*/, ctl::ValueStore& /*store*/,
                       const ctl::DrawOptions& /*options*/,
                       const std::string& /*section*/,
                       const ctl::InstanceKey& /*key*/, bool& /*changed*/) {
  DLSSNR_PROFILE_UI_L3("DrawTemporalStats");
  IndentScope indent;
  const temporal_stats::Snapshot& snap = temporal_stats::Get();  auto row = [](const char* label, const ImVec4& color, const char* value,
                const char* tooltip) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(p.x + h * 0.5f, p.y + h * 0.5f), h * 0.35f,
        ImGui::GetColorU32(color));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s: %s", label, value);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("%s", tooltip);
    }
  };

  if (!snap.active) {
    row("Temporal", ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "idle",
        "No pass has run a temporal step since the addon was last reloaded.");
    return true;
  }

  const ImVec4 kGood(0.4f, 0.9f, 0.4f, 1.0f);
  const ImVec4 kWarn(0.95f, 0.75f, 0.3f, 1.0f);
  const ImVec4 kBad(0.9f, 0.4f, 0.4f, 1.0f);

  const uint64_t total = snap.reuse_frames + snap.refresh_frames;
  const float reuse_percent = total == 0
      ? 0.0f
      : 100.0f * static_cast<float>(snap.reuse_frames) /
            static_cast<float>(total);

  char buf[128];
  std::snprintf(buf, sizeof(buf), "%s · age %llu/%llu",
                snap.reuse ? "reuse (NGX skipped)" : "refresh (NGX ran)",
                static_cast<unsigned long long>(snap.age),
                static_cast<unsigned long long>(snap.cadence));
  row("This frame", snap.reuse ? kGood : kWarn, buf,
      "Whether the model was evaluated this frame. The age/cadence pair is "
      "the frames since the last evaluation over the effective reuse window.");

  std::snprintf(buf, sizeof(buf), "%.1f%% (%llu reuse / %llu refresh)",
                static_cast<double>(reuse_percent),
                static_cast<unsigned long long>(snap.reuse_frames),
                static_cast<unsigned long long>(snap.refresh_frames));
  row("Reuse ratio", kGood, buf,
      "Share of frames that skipped the model since the addon loaded.");

  std::snprintf(buf, sizeof(buf), "%.2f%%%s",
                static_cast<double>(snap.invalid_fraction * 100.0f),
                snap.feedback_valid ? "" : " (no sample yet)");
  row("Invalid pixels",
      snap.invalid_fraction > 0.15f ? kBad
                                    : (snap.invalid_fraction > 0.05f ? kWarn : kGood),
      buf,
      "Pixels whose reprojected edit fell below the reuse confidence "
      "threshold. Delayed by the readback, so it lags the current frame.");

  if (snap.gpu_valid) {
    std::snprintf(buf, sizeof(buf), "conf %.3f · |edit| %.4f",
                  static_cast<double>(snap.mean_conf),
                  static_cast<double>(snap.mean_edit));
    row("GPU sample", snap.mean_conf < 0.75f ? kWarn : kGood, buf,
        "Mean candidate confidence and mean absolute log-gain of the "
        "reprojected edit over the last completed GPU sample.");
  } else {
    row("GPU sample", kBad, "unavailable",
        "The stats readback has not completed yet. Reuse stays gated until it does.");
  }

  if (snap.dt_reuse_ms > 0.0f && snap.dt_refresh_ms > 0.0f) {
    const float step = snap.dt_refresh_ms - snap.dt_reuse_ms;
    std::snprintf(buf, sizeof(buf), "reuse %.2f ms · refresh %.2f ms · step %+.2f ms",
                  static_cast<double>(snap.dt_reuse_ms),
                  static_cast<double>(snap.dt_refresh_ms),
                  static_cast<double>(step));
    row("Frame time", step > 4.0f ? kBad : (step > 2.0f ? kWarn : kGood), buf,
        "Smoothed frame interval for each class. A large step is judder: the "
        "eye sees it as discontinuity no matter how high the average fps is.");
  } else {
    row("Frame time", kWarn, "warming up",
        "Needs at least one frame of each class.");
  }

  if (snap.cadence_short) {
    std::snprintf(buf, sizeof(buf),
                  "halved to %llu (invalid above the configured maximum)",
                  static_cast<unsigned long long>(snap.cadence));
    row("Cadence", kWarn, buf,
        "The invalid fraction exceeded Max Invalid %, so the reuse window was "
        "shortened. Latched with hysteresis so it cannot flip every frame.");
  }
  return true;
}

}  // namespace pass
}  // namespace feature

}  // namespace reshade_dlssnr

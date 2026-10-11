#include <imgui.h>
#include <reshade.hpp>

#include "ui/ui_sections.h"

#include <cstdio>

#include "addon_controller.h"
#include "state/settings.h"
#include "status.h"
#include "ui/ui_widgets.h"

namespace reshade_dlssnr {

bool AnyPassUsesHdrInjection(const Settings& settings) {
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (settings.pass_enabled[i] &&
        settings.passes[i].injection_stage != InjectionStage::Backbuffer) {
      return true;
    }
  }
  return false;
}

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
  if (!PersistentHeader(
          "Status", settings.ui_status_open, changed,
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

  const bool hdr_in_use = AnyPassUsesHdrInjection(settings);
  const bool hdr_good = !settings.hdr.enable || !hdr_in_use || snap.hdr_ready;
  char hdr_buf[96];
  if (!settings.hdr.enable) {
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

  if (settings.hdr.enable && hdr_in_use && snap.gain_map_ready) {
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

  {
    bool any_failed = false;
    for (uint32_t i = 0; i < snap.pass_count; ++i) {
      if (snap.passes[i].failed) { any_failed = true; break; }
    }

    if (any_failed) {
      ImGui::Spacing();
      if (ImGui::Button("Retry failed passes")) {
        AddonController::Get().RetryAll();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Clear every failed pass and rebuild its model on the next frame.");
      }
    }
  }

  ImGui::Separator();

  // Match the runtime's established execution grouping: Pre-DLSS, then
  // Post-DLSS, then Backbuffer. Within each target group, retain pass/tab
  // order. Status remains indexed by the original pass ID, so rows display
  // the original Pass N label and look up the matching settings/status.
  constexpr InjectionStage kStatusStageOrder[] = {
      InjectionStage::PreDLSS,
      InjectionStage::PostDLSS,
      InjectionStage::Backbuffer,
  };
  for (const InjectionStage display_stage : kStatusStageOrder) {
    for (uint32_t i = 0; i < snap.pass_count; ++i) {
      if (settings.passes[i].injection_stage != display_stage) continue;

      char buf[kStatusTextLength];
      FormatPassStatus(snap.passes[i], settings.passes[i].injection_stage,
                       GetResolvedPass(i).working_scale,
                       snap.post_dlss_blocked, buf, sizeof(buf));

      char label[32];
      std::snprintf(label, sizeof(label), "Pass %u", i + 1);

      const bool stage_blocked =
          snap.post_dlss_blocked &&
          settings.passes[i].injection_stage == InjectionStage::PostDLSS;
      const bool ok = snap.passes[i].enabled && !snap.passes[i].failed &&
                      !stage_blocked;

      bullet(ok);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Original pass index %u. Display order is grouped Pre-DLSS, Post-DLSS, then Backbuffer while preserving pass order inside each group.", i + 1);
      }

      ImGui::SameLine(kControlLabelWidth);
      ImGui::TextUnformatted(buf);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Current runtime status for this pass, including its target stage, working scale, initialization state, and any failure reason.");
      }
    }
  }
}

}  // namespace reshade_dlssnr

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
#include "gpu/d3d12_utils.h"
#include "state/settings.h"
#include "status.h"
#include "temporal/stats.h"
#include "ui/ui_widgets.h"
#include "feature/ui_state/settings.h"

namespace reshade_dlssnr {


namespace {

void FormatBytes(uint64_t bytes, char* out, size_t out_size) {
  if (bytes >= 1024ull * 1024 * 1024) {
    std::snprintf(out, out_size, "%.2f GB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 1024ull * 1024) {
    std::snprintf(out, out_size, "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(out, out_size, "%.1f KB",
                  static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(out, out_size, "%llu B",
                  static_cast<unsigned long long>(bytes));
  }
}

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
    std::snprintf(scale, sizeof(scale), " - %.3gx",
                  static_cast<double>(working_scale));
  }

  if (!pass.enabled) {
    std::snprintf(out, out_size, "Disabled");
    return;
  }
  if (post_dlss_blocked && stage == InjectionStage::PostDLSS) {
    std::snprintf(out, out_size, "%s - blocked%s", stage_name, scale);
    return;
  }
  if (pass.failed) {
    std::snprintf(out, out_size, "%s - FAILED: %s%s",
                  stage_name,
                  pass.reason[0] != '\0' ? pass.reason : "unknown error",
                  scale);
    return;
  }
  if (!pass.has_feature) {
    std::snprintf(out, out_size, "%s - initializing%s", stage_name, scale);
    return;
  }
  std::snprintf(out, out_size, "%s - running%s", stage_name, scale);
}

}  // namespace

void DrawStatusSection(Settings& settings, bool& changed) {
  DLSSNR_PROFILE_UI_L2("DrawStatusSection");
  DrawCollapsible(
      "Status", feature::ui_state::kStatusOpen, changed,
      "Live addon, NGX, guide, HDR, and per-pass health. Pass rows use the same target-stage grouping as execution.",
      [&] {
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
    std::snprintf(runtime_buf, sizeof(runtime_buf), "Not initialized - %s",
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
    std::snprintf(hdr_buf, sizeof(hdr_buf), "Active - exp %.3f - white %.3f",
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
    ImGui::Text("Gain map: %.3f .. %.3f log2 - SDR excursions: %u - gain-range: %u",
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

  // GPU resource breakdown. Collapsed by default so the fast-glance health
  // and per-pass status stay at the top of the panel; the numbers here matter
  // when tuning Working Scale / x2 or diagnosing VRAM pressure.
  ImGui::Separator();
  if (PersistentSettingHeader(
          "GPU Resources", feature::ui_state::kResourcesOpen, changed,
          "Currently allocated per-pass GPU resources. Totals do not include "
          "the shared compute engine's descriptor heap, its constant buffer, "
          "or the release command context.")) {
    IndentScope resources_indent;

    char total_buf[32];
    FormatBytes(snap.total_vram_bytes, total_buf, sizeof(total_buf));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s across %u pass%s", total_buf,
                static_cast<unsigned>(snap.passes.size()),
                snap.passes.size() == 1 ? "" : "es");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      Tip("Sum of every live pass's allocated GPU resources at the last "
          "refresh. Reallocations are not counted twice; retired resources "
          "waiting on the GPU fence are shown separately below.");
    }

    // Retired resources sit directly under the total: they are memory the
    // addon still holds but which is not part of any pass's total_bytes, and
    // it is what makes the total appear to lag after a scale change.
    if (snap.retired_resources != 0) {
      ImGui::TextDisabled("Pending release: %u", snap.retired_resources);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        Tip("Resources retired on a reallocation that are still alive until "
            "the GPU queue fence for their last reader completes.");
      }
    }

    // Per-pass tables. Skipped for passes that have not produced a session
    // yet, since their resources are genuinely empty rather than zero-sized.
    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable;
    const ImGuiTreeNodeFlags pass_flags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;

    for (const PassStatusEntry& entry : snap.passes) {
      if (entry.status.resources.count == 0) continue;

      const auto position_it = position_by_id.find(entry.id);
      const uint32_t position =
          position_it == position_by_id.end()
              ? 0u : static_cast<uint32_t>(position_it->second + 1);

      const feature::pass::PassSnapshot pass = feature::pass::Read(
          feature::SettingsStore(), feature::pass::PassKey(entry.id));
      const char* stage_name =
          pass.target == 0 ? "Pre-DLSS" :
          pass.target == 1 ? "Post-DLSS" : "Backbuffer";

      char pass_label[96];
      char pass_bytes[32];
      FormatBytes(entry.status.resources.total_bytes, pass_bytes,
                  sizeof(pass_bytes));
      std::snprintf(pass_label, sizeof(pass_label), "Pass %u (%s) - %s",
                    position, stage_name, pass_bytes);

      // The tree node keeps its own per-pass ID; the table is drawn after the
      // node is popped, because an open node pushes its ID and ImGui saves
      // column widths per table ID. Outside that scope every pass resolves to
      // one table, so resizing one column resizes it in all of them.
      ImGui::PushID(static_cast<int>(entry.id));
      const bool resource_tree_open = ImGui::TreeNodeEx(
          "##pass_resources", pass_flags, "%s", pass_label);
      ImGui::PopID();
      if (resource_tree_open) {
        ImGui::TreePop();
        ImGui::Indent();
        if (ImGui::BeginTable("pass_resource_table", 4, table_flags)) {
          ImGui::TableSetupColumn("Resource",
              ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Dims",
              ImGuiTableColumnFlags_WidthFixed, 90.0f);
          ImGui::TableSetupColumn("Format",
              ImGuiTableColumnFlags_WidthFixed, 150.0f);
          ImGui::TableSetupColumn("Size",
              ImGuiTableColumnFlags_WidthFixed, 80.0f);
          ImGui::TableHeadersRow();

          for (uint32_t i = 0; i < entry.status.resources.count; ++i) {
            const ResourceInfoEntry& res = entry.status.resources.entries[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(res.name);

            ImGui::TableSetColumnIndex(1);
            if (res.height == 0) {
              ImGui::Text("%u B", res.width);
            } else {
              ImGui::Text("%ux%u", res.width, res.height);
            }

            ImGui::TableSetColumnIndex(2);
            if (res.height == 0) {
              ImGui::TextDisabled("buffer");
            } else {
              ImGui::TextUnformatted(
                  FormatName(static_cast<DXGI_FORMAT>(res.format)));
            }

            ImGui::TableSetColumnIndex(3);
            char entry_buf[32];
            FormatBytes(res.bytes, entry_buf, sizeof(entry_buf));
            ImGui::TextUnformatted(entry_buf);
          }

          ImGui::EndTable();
        }
        ImGui::Unindent();
      }
    }
  }
  });
}

namespace feature {
namespace pass {

// Hook body for the last row of the Temporal Reprojection category. The
// category header owns the collapsible; this only draws the readout.
//
// Every row is always drawn. Rows that appear only once data exists change the
// block's height while the user is watching it, which shifts everything below
// the category on and off the cursor.
bool DrawTemporalStats(const void* /*context*/, ctl::ValueStore& store,
                       const ctl::DrawOptions& /*options*/,
                       const std::string& section,
                       const ctl::InstanceKey& key, bool& /*changed*/) {
  DLSSNR_PROFILE_UI_L3("DrawTemporalStats");
  if (store.Read(section, key, kTemporalMode).as_int == 0) return true;

  IndentScope indent;
  const temporal_stats::Snapshot& snap = temporal_stats::Get();
  auto row = [](const char* label, const ImVec4& color, const char* value,
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

  const ImVec4 kGood(0.4f, 0.9f, 0.4f, 1.0f);
  const ImVec4 kWarn(0.95f, 0.75f, 0.3f, 1.0f);
  const ImVec4 kBad(0.9f, 0.4f, 0.4f, 1.0f);
  const ImVec4 kIdle(0.6f, 0.6f, 0.6f, 1.0f);

  char buf[128];
  if (!snap.active) {
    row("This frame", kIdle, "idle",
        "No pass has run a temporal step since the addon was last reloaded.");
  } else {
    std::snprintf(buf, sizeof(buf), "%s - age %llu/%llu",
                  snap.reuse ? "reuse (NGX skipped)" : "refresh (NGX ran)",
                  static_cast<unsigned long long>(snap.age),
                  static_cast<unsigned long long>(snap.cadence));
    row("This frame", snap.reuse ? kGood : kWarn, buf,
        "Whether the model was evaluated this frame. The age/cadence pair is "
        "the frames since the last evaluation over the effective reuse window.");
  }

  const uint64_t total = snap.reuse_frames + snap.refresh_frames;
  if (total == 0) {
    row("Reuse ratio", kIdle, "no frames yet",
        "Share of frames that skipped the model since the addon loaded.");
  } else {
    const float reuse_percent = 100.0f *
        static_cast<float>(snap.reuse_frames) / static_cast<float>(total);
    std::snprintf(buf, sizeof(buf), "%.1f%% (%llu reuse / %llu refresh)",
                  static_cast<double>(reuse_percent),
                  static_cast<unsigned long long>(snap.reuse_frames),
                  static_cast<unsigned long long>(snap.refresh_frames));
    row("Reuse ratio", kGood, buf,
        "Share of frames that skipped the model since the addon loaded.");
  }

  if (!snap.feedback_valid) {
    row("Invalid pixels", kIdle, "no sample yet",
        "Pixels whose reprojected edit fell below the reuse confidence "
        "threshold. Delayed by the readback, so it lags the current frame.");
  } else {
    std::snprintf(buf, sizeof(buf), "%.2f%%",
                  static_cast<double>(snap.invalid_fraction * 100.0f));
    row("Invalid pixels",
        snap.invalid_fraction > 0.15f ? kBad
                                      : (snap.invalid_fraction > 0.05f ? kWarn : kGood),
        buf,
        "Pixels whose reprojected edit fell below the reuse confidence "
        "threshold. Delayed by the readback, so it lags the current frame.");
  }

  if (!snap.gpu_valid) {
    row("GPU sample", kIdle, "no sample yet",
        "Mean candidate confidence and mean absolute log gain of the "
        "reprojected edit over the last completed GPU sample.");
  } else {
    std::snprintf(buf, sizeof(buf), "conf %.3f - |edit| %.4f",
                  static_cast<double>(snap.mean_conf),
                  static_cast<double>(snap.mean_edit));
    row("GPU sample", snap.mean_conf < 0.75f ? kWarn : kGood, buf,
        "Mean candidate confidence and mean absolute log gain of the "
        "reprojected edit over the last completed GPU sample.");
  }

  if (snap.dt_reuse_ms <= 0.0f || snap.dt_refresh_ms <= 0.0f) {
    row("Frame time", kIdle, "needs one frame of each class",
        "Smoothed frame interval for each class. A large step is judder: the "
        "eye sees it as discontinuity no matter how high the average fps is.");
  } else {
    const float step = snap.dt_refresh_ms - snap.dt_reuse_ms;
    std::snprintf(buf, sizeof(buf), "reuse %.2f ms - refresh %.2f ms - step %+.2f ms",
                  static_cast<double>(snap.dt_reuse_ms),
                  static_cast<double>(snap.dt_refresh_ms),
                  static_cast<double>(step));
    row("Frame time", step > 4.0f ? kBad : (step > 2.0f ? kWarn : kGood), buf,
        "Smoothed frame interval for each class. A large step is judder: the "
        "eye sees it as discontinuity no matter how high the average fps is.");
  }

  if (!snap.cadence_short) {
    std::snprintf(buf, sizeof(buf), "configured window (%llu)",
                  static_cast<unsigned long long>(snap.cadence));
    row("Cadence", kGood, buf,
        "The invalid fraction exceeded Max Invalid %, so the reuse window was "
        "shortened. Latched with hysteresis so it cannot flip every frame.");
  } else {
    std::snprintf(buf, sizeof(buf), "halved to %llu",
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

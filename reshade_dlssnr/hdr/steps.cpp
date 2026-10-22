#include "pipeline/pass_session.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cmath>

#include "hdr/hdr_state.h"
#include "debug/debug_view.h"
#include "model/model_input.h"
#include "pipeline_trace.h"

#include "pipeline/pipeline_graph.h"

namespace reshade_dlssnr {

PipelineStatus PassSession::StepBuildHdrUniforms(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("PassSession::StepBuildHdrUniforms");
  run.hdr_uniforms_ready = false;
  if (run.hdr == nullptr) return PipelineStatus::Ok;
  if (!run.hdr->Enabled()) return PipelineStatus::Ok;

  const HdrSettings& hs = run.hdr_state;
  const LocalExposureLayerSettings& local = hs.local_exposure;
  run.hdr_uniforms.format = static_cast<float>(HdrFormatCode(run.hdr->ResolvedFormat()));
  run.hdr_uniforms.exposure = run.hdr->Exposure();
  const float bt2446_k1 = std::max(hs.bt2446_k1, 1e-6f);
  const float bt2446_k3 = std::clamp(hs.bt2446_k3, 0.0f, 1.0f);
  const float bt2446_ip = std::max(hs.bt2446_ip, 1e-6f);
  const float bt2446_remaining = std::max(1.0f - bt2446_k3, 1e-6f);
  run.hdr_uniforms.bt2446_k1 = bt2446_k1;
  run.hdr_uniforms.bt2446_k2 = bt2446_k1 * bt2446_ip * (1.0f - bt2446_k3);
  run.hdr_uniforms.bt2446_k3 = bt2446_k3;
  run.hdr_uniforms.bt2446_k4 = bt2446_k1 * bt2446_ip *
      (1.0f - (1.0f - bt2446_k3) * std::log(bt2446_remaining));
  run.hdr_uniforms.bt2446_ip = bt2446_ip;
  run.hdr_uniforms.bt2446_crosstalk = hs.bt2446_crosstalk;
  run.hdr_uniforms.gain_min_log2 = hs.gain_min_log2;
  run.hdr_uniforms.gain_max_log2 = hs.gain_max_log2;
  run.hdr_uniforms.effective_scene_white_nits = run.hdr->EffectiveSceneWhiteNits(hs);
  run.hdr_uniforms.local_exposure_strength = local.enabled ? local.strength : 0.0f;
  run.hdr_uniforms.local_max_brighten = local.max_brighten;
  run.hdr_uniforms.local_max_darken = local.max_darken;
  run.hdr_uniforms.midtone = local.midtone;
  run.hdr_uniforms.local_shadow_strength = local.shadow_strength;
  run.hdr_uniforms.local_highlight_strength = local.highlight_strength;
  run.hdr_uniforms.local_shadow_range = local.shadow_range;
  run.hdr_uniforms.local_highlight_range = local.highlight_range;
  run.hdr_uniforms.local_highlight_protection = local.highlight_protection;
  run.hdr_uniforms.local_shadow_protection = local.shadow_protection;
  run.hdr_uniforms.local_detail_retention = local.detail_retention;
  run.hdr_uniforms.local_edge_protection = local.edge_protection;
  run.hdr_uniforms.local_radius_px = local.radius_px;
  run.hdr_uniforms.local_samples = local.samples;
  run.hdr_uniforms.restore_local_exposure = local.restore_exposure;
  run.hdr_uniforms.restore_local_mode = local.restore_mode;
  run.hdr_uniforms_ready = true;
  return PipelineStatus::Ok;
}


namespace {
void BindLocalExposureInputs(const PassRun& run, ComputeConstants& constants,
                             ID3D12Resource* source, ID3D12Resource** srvs) {
  constants.hdr.LocalExposureParams.x = run.hdr_uniforms.local_radius_px;
  constants.hdr.LocalExposureParams.y =
      static_cast<float>(run.hdr_uniforms.local_samples);
  constants.hdr.LocalExposureParams.z = 0.0f;

  srvs[0] = source;
  srvs[1] = source;
  srvs[2] = source;
  srvs[3] = source;
}
}  // namespace
PipelineStatus PassSession::StepGainMapSplit(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepGainMapSplit");
  PassSession* self = run.session;
  if (self == nullptr || run.codec == nullptr || run.list == nullptr ||
      !run.hdr_uniforms_ready || run.hdr == nullptr ||
      run.hdr->ResolvedFormat() == HdrFormat::Sdr) {
    return PipelineStatus::Ok;
  }

  ID3D12Resource* source = run.model_input;
  ID3D12Resource* packed = self->resources_.GetPackedOutput();
  ID3D12Resource* stats = self->resources_.GetGainStats();
  if (source == nullptr || packed == nullptr) {
    self->FailForStep("HDR gain-map resources are unavailable");
    return PipelineStatus::Fail;
  }

  ComputeConstants constants = {};
  constants.Width = run.width;
  constants.Height = run.height;
  constants.hdr.Format = run.hdr_uniforms.format;
  constants.hdr.Exposure = run.hdr_uniforms.exposure;
  constants.hdr.Bt2446K1 = run.hdr_uniforms.bt2446_k1;
  constants.hdr.Bt2446K2 = run.hdr_uniforms.bt2446_k2;
  constants.hdr.Bt2446K3 = run.hdr_uniforms.bt2446_k3;
  constants.hdr.Bt2446K4 = run.hdr_uniforms.bt2446_k4;
  constants.hdr.Bt2446Ip = run.hdr_uniforms.bt2446_ip;
  constants.hdr.Crosstalk = run.hdr_uniforms.bt2446_crosstalk;
  constants.hdr.GainMinLog2 = run.hdr_uniforms.gain_min_log2;
  constants.hdr.GainMaxLog2 = run.hdr_uniforms.gain_max_log2;
  constants.hdr.EffectiveSceneWhiteNits = run.hdr_uniforms.effective_scene_white_nits;
  model_pipeline::SetLocalExposureConstants(constants, run.hdr_uniforms);

  ID3D12Resource* global_carrier = nullptr;
  if (run.hdr_uniforms.local_exposure_strength > 0.0f) {
    global_carrier = self->resources_.GetGlobalHdrCarrier();
    if (global_carrier == nullptr) {
      self->FailForStep("unlifted HDR carrier resource is unavailable");
      return PipelineStatus::Fail;
    }
    constants.shader.Params1.x = 1.0f;
  }

  ID3D12Resource* srvs[kSrvCount] = {};
  BindLocalExposureInputs(run, constants, source, srvs);
  ID3D12Resource* uavs[kUavCount] = {packed, global_carrier};
  ResourceTransition split_transitions[] = {
      {source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
      {packed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
      {global_carrier, D3D12_RESOURCE_STATE_UNORDERED_ACCESS},
  };
  self->command_.TransitionBatch(run.list, split_transitions,
                                 3);
  if (!run.codec->Dispatch("GainMapSplit", run.list, constants, srvs, uavs)) {
    self->FailForStep("failed to split HDR into SDR + gain map");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, packed);
  self->command_.Transition(run.list, packed,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (global_carrier != nullptr) {
    self->command_.UavBarrier(run.list, global_carrier);
    self->command_.Transition(run.list, global_carrier,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  // The detailed statistics and counters are pass-owned, so every HDR pass
  // may collect them when the opt-in setting is enabled.
  if (!run.hdr_state.detailed_statistics) {
    return PipelineStatus::Ok;
  }
  if (stats == nullptr) {
    self->FailForStep("HDR gain-map statistics resource is unavailable");
    return PipelineStatus::Fail;
  }
  ID3D12Resource* stats_srvs[kSrvCount] = {packed, packed, packed, packed};
  ID3D12Resource* stats_uavs[kUavCount] = {stats};
  self->command_.Transition(run.list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComputeConstants stat_constants = constants;
  if (!run.codec->Dispatch("ZeroGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs, 1, 1, 1)) {
    self->FailForStep("failed to initialize HDR gain-map statistics");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, stats);
  if (!run.codec->Dispatch("BuildGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs)) {
    self->FailForStep("failed to build HDR gain-map statistics");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(run.list, stats);
  self->command_.Transition(run.list, stats, D3D12_RESOURCE_STATE_COPY_SOURCE);

  // Every pass owns its own HDR stats/readback state.
  if (run.hdr != nullptr) {
    if (!run.hdr->EnqueueGainMapStatsReadback(run.list, stats)) {
      TRACE_EVT(self, "gain_stats_readback unavailable");
    }
  }

  self->command_.Transition(run.list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  return PipelineStatus::Ok;
}
// Builds the full-resolution local SDR exposure scale for debug visualization.
// HDR inverse composition recovers the same multiplier from the source/carrier
// pair and does not use this extra texture in its descriptor table.
PipelineStatus PassSession::StepBuildEffectiveLocalExposure(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepBuildEffectiveLocalExposure");
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;

  const bool selected_active_view = run.settings != nullptr &&
      run.settings->active_pass_id == self->pass_id_ &&
      DebugViewNeedsEffectiveExposureMap(run.debug_view);
  const bool local_hdr_active = run.codec != nullptr && run.hdr != nullptr &&
      run.hdr_uniforms_ready && run.hdr->ResolvedFormat() != HdrFormat::Sdr &&
      run.model_input != nullptr &&
      run.hdr_uniforms.local_exposure_strength > 0.0f;
  if (!selected_active_view || !local_hdr_active) {
    self->resources_.ReleaseEffectiveExposureMap();
    return PipelineStatus::Ok;
  }

  if (!self->resources_.EnsureEffectiveExposureMap(
          run.device, run.width, run.height)) {
    self->FailForStep("failed to create HDR effective exposure map");
    return PipelineStatus::Fail;
  }

  ComputeConstants constants = {};
  constants.Width = run.width;
  constants.Height = run.height;
  constants.hdr.Format = run.hdr_uniforms.format;
  constants.hdr.Exposure = run.hdr_uniforms.exposure;
  constants.hdr.Bt2446K1 = run.hdr_uniforms.bt2446_k1;
  constants.hdr.Bt2446K2 = run.hdr_uniforms.bt2446_k2;
  constants.hdr.Bt2446K3 = run.hdr_uniforms.bt2446_k3;
  constants.hdr.Bt2446K4 = run.hdr_uniforms.bt2446_k4;
  constants.hdr.Bt2446Ip = run.hdr_uniforms.bt2446_ip;
  constants.hdr.Crosstalk = run.hdr_uniforms.bt2446_crosstalk;
  constants.hdr.GainMinLog2 = run.hdr_uniforms.gain_min_log2;
  constants.hdr.GainMaxLog2 = run.hdr_uniforms.gain_max_log2;
  constants.hdr.EffectiveSceneWhiteNits =
      run.hdr_uniforms.effective_scene_white_nits;
  model_pipeline::SetLocalExposureConstants(constants, run.hdr_uniforms);

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  ID3D12Resource* srvs[kSrvCount] = {};
  BindLocalExposureInputs(run, constants, run.model_input, srvs);
  ID3D12Resource* exposure_map = self->resources_.GetEffectiveExposureMap();
  ID3D12Resource* uavs[kUavCount] = {exposure_map};
  self->command_.Transition(list, run.model_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, exposure_map,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  if (!run.codec->Dispatch("BuildEffectiveLocalExposure", list, constants,
                           srvs, uavs)) {
    self->FailForStep("failed to build HDR effective local exposure map");
    return PipelineStatus::Fail;
  }
  self->command_.UavBarrier(list, exposure_map);
  self->command_.Transition(list, exposure_map,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

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
  // Local exposure does not depend on the HDR path, so its uniforms are filled
  // before the gates that describe HDR itself. Everything below this point
  // needs a live HDR source.
  model_pipeline::SetLocalExposureUniforms(run.hdr_uniforms,
                                           run.local_exposure);
  if (run.hdr == nullptr) return PipelineStatus::Ok;
  if (!run.hdr->Enabled()) return PipelineStatus::Ok;

  const HdrSettings& hs = run.hdr_state;
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
    return self->FailStep("HDR gain-map resources are unavailable");
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
      return self->FailStep("unlifted HDR carrier resource is unavailable");
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
    return self->FailStep("failed to split HDR into SDR + gain map");
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
    return self->FailStep("HDR gain-map statistics resource is unavailable");
  }
  ID3D12Resource* stats_srvs[kSrvCount] = {packed, packed, packed, packed};
  ID3D12Resource* stats_uavs[kUavCount] = {stats};
  self->command_.Transition(run.list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ComputeConstants stat_constants = constants;
  if (!run.codec->Dispatch("ZeroGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs, 1, 1, 1)) {
    return self->FailStep("failed to initialize HDR gain-map statistics");
  }
  self->command_.UavBarrier(run.list, stats);
  if (!run.codec->Dispatch("BuildGainMapStats", run.list, stat_constants,
                           stats_srvs, stats_uavs)) {
    return self->FailStep("failed to build HDR gain-map statistics");
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
    return self->FailStep("failed to create HDR effective exposure map");
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
    return self->FailStep("failed to build HDR effective local exposure map");
  }
  self->command_.UavBarrier(list, exposure_map);
  self->command_.Transition(list, exposure_map,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

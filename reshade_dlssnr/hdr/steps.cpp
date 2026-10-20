#include "pipeline/pass_session.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cmath>

#include "hdr/hdr_state.h"
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
  run.hdr_uniforms.fp16_unlifted_reference = local.fp16_reference;
  run.hdr_uniforms_ready = true;
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

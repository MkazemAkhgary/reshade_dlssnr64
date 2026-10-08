#include "pipeline/pass_session.h"

#include <algorithm>

#include "hdr/hdr_state.h"
#include "pipeline/pipeline_graph.h"

namespace reshade_dlssnr {

PipelineStatus PassSession::StepBuildHdrUniforms(PassRun& run) {
  PassSession* self = run.session;
  run.hdr_uniforms_ready = false;
  if (run.hdr == nullptr || run.settings == nullptr) return PipelineStatus::Ok;
  if (!run.hdr->Enabled()) return PipelineStatus::Ok;

  const HdrSettings& hs = run.settings->hdr;
  run.hdr_uniforms.format = static_cast<float>(HdrFormatCode(run.hdr->ResolvedFormat()));
  run.hdr_uniforms.exposure = run.hdr->Exposure();
  run.hdr_uniforms.bt2446_k1 = hs.bt2446_k1;
  run.hdr_uniforms.bt2446_k2 = hs.bt2446_k2;
  run.hdr_uniforms.bt2446_k3 = hs.bt2446_k3;
  run.hdr_uniforms.bt2446_k4 = hs.bt2446_k4;
  run.hdr_uniforms.bt2446_crosstalk = hs.bt2446_crosstalk;
  run.hdr_uniforms.gain_base_offset = hs.gain_base_offset;
  run.hdr_uniforms.gain_alternate_offset = hs.gain_alternate_offset;
  run.hdr_uniforms.gain_min_log2 = hs.gain_min_log2;
  run.hdr_uniforms.gain_max_log2 = hs.gain_max_log2;
  run.hdr_uniforms.effective_scene_white_nits =
      run.hdr->EffectiveSceneWhiteNits(hs);
  run.hdr_uniforms.local_exposure_strength =
      hs.exposure_method == HdrExposureMethod::AutoLocal
          ? hs.local_exposure_strength
          : 0.0f;
  run.hdr_uniforms.local_max_brighten = hs.local_max_brighten;
  run.hdr_uniforms.local_max_darken = hs.local_max_darken;
  run.hdr_uniforms.midtone = hs.midtone;
  run.hdr_uniforms_ready = true;
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

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

  const Settings& settings = *run.settings;
  run.hdr_uniforms.exposure = run.hdr->Exposure();
  run.hdr_uniforms.tonemap_white =
      run.hdr->SceneWhite() * std::max(settings.hdr.highlight_headroom, 0.01f);
  run.hdr_uniforms.format = HdrFormatCode(run.hdr->ResolvedFormat());
  run.hdr_uniforms_ready = true;
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

#include "pipeline/pass_session.h"

#include "runtime/debug_view.h"
#include "pipeline/pipeline_graph.h"
#include "skin/skin_detect.h"

namespace reshade_dlssnr {

PipelineStatus PassSession::StepRenderDebugView(PassRun& run) {
  PassSession* self = run.session;
  if (run.settings == nullptr || run.codec == nullptr ||
      run.frame_debug == nullptr) {
    return PipelineStatus::Ok;
  }

  DebugSnapshot debug;
  debug.source = run.stage != nullptr && run.stage->is_post_dlss
                     ? run.model_input
                     : run.backbuffer;
  debug.source_width = run.width;
  debug.source_height = run.height;
  debug.prepared_input = run.model_color;
  debug.prepared_input_width = run.eval_width;
  debug.prepared_input_height = run.eval_height;
  debug.model_output = run.model_output;
  debug.model_output_width = run.eval_width;
  debug.model_output_height = run.eval_height;
  debug.composed_output = self->resources_.GetCompositeOutput();
  debug.composed_output_width = run.width;
  debug.composed_output_height = run.height;
  if (run.skin_detect != nullptr) {
    const SkinDetect::Output& sd = run.skin_detect->GetOutput();
    if (sd.valid) {
      debug.skin_detect_input = sd.detect_input;
      debug.skin_detect_output = sd.detect_output;
      debug.skin_mask = sd.mask;
      debug.skin_detect_width = sd.width;
      debug.skin_detect_height = sd.height;
    }
  }
  if (run.guides_valid) {
    debug.motion = run.guides.motion;
    debug.motion_width = run.guides.guide_width;
    debug.motion_height = run.guides.guide_height;
    debug.depth = run.guides.depth;
    debug.depth_width = run.guides.guide_width;
    debug.depth_height = run.guides.guide_height;
  }

  self->MaybeRenderDebug(*run.settings, *run.codec,
                         self->StageList(run, self->command_),
                         *run.frame_debug, debug, run.width, run.height,
                         run.desc.Format);
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

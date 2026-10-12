#include "pipeline/pass_session.h"

#include "debug/debug_view.h"
#include "pipeline/pipeline_graph.h"
#include "importance_mask/importance_mask.h"

namespace reshade_dlssnr {

PipelineStatus PassSession::StepRenderDebugView(PassRun& run) {
  PassSession* self = run.session;
  if (run.settings == nullptr || run.codec == nullptr ||
      run.frame_debug == nullptr) {
    return PipelineStatus::Ok;
  }

  DebugSnapshot debug;
  debug.source = run.stage != nullptr && !run.stage->owns_submit
                     ? run.model_input
                     : run.backbuffer;
  debug.source_width = run.width;
  debug.source_height = run.height;
  if (run.hdr != nullptr) {
    const HdrSnapshot hdr_snapshot = run.hdr->GetSnapshot();
    debug.game_exposure_available = hdr_snapshot.game_exposure_available;
    debug.game_exposure = hdr_snapshot.game_exposure;
  }
  if (run.hdr_uniforms_ready && run.hdr != nullptr &&
      run.hdr->ResolvedFormat() != HdrFormat::Sdr) {
    debug.packed_hdr_sdr = self->resources_.GetPackedOutput();
    debug.packed_hdr_gain = debug.packed_hdr_sdr;
    debug.packed_width = run.width;
    debug.packed_height = run.height;
  }
  debug.prepared_input = run.model_color;
  debug.prepared_input_width = run.eval_width;
  if (run.hdr_uniforms_ready && run.hdr != nullptr &&
      run.hdr->ResolvedFormat() != HdrFormat::Sdr) {
    debug.effective_exposure_map = self->resources_.GetEffectiveExposureMap();
    if (debug.effective_exposure_map != nullptr) {
      debug.effective_exposure_width = run.width;
      debug.effective_exposure_height = run.height;
    }
  }
  debug.prepared_input_height = run.eval_height;
  if (self->resources_.GetScratch(kDebugPreGradeInputSlot) != nullptr) {
    debug.pre_grade_input = self->resources_.GetScratch(kDebugPreGradeInputSlot);
    debug.pre_grade_input_width = run.eval_width;
    debug.pre_grade_input_height = run.eval_height;
  }
  if (self->resources_.GetScratch(kDebugPreGradeOutputSlot) != nullptr) {
    debug.pre_grade_output = self->resources_.GetScratch(kDebugPreGradeOutputSlot);
    debug.pre_grade_output_width = run.eval_width;
    debug.pre_grade_output_height = run.eval_height;
  }
  debug.model_output = run.model_output;
  debug.model_output_width = run.eval_width;
  debug.model_output_height = run.eval_height;
  if (run.pass != nullptr) {
    debug.rcas_enabled = run.pass->rcas_enabled;
    debug.rcas_strength = run.pass->rcas_strength;
    debug.input_grade = run.pass->grade_input;
  }
  debug.composed_output = self->resources_.GetCompositeOutput();
  debug.composed_output_width = run.width;
  debug.composed_output_height = run.height;
  debug.importance_width = run.eval_width;
  debug.importance_height = run.eval_height;
  if (run.importance_mask != nullptr) {
    const ImportanceMask::Output mask = run.importance_mask->GetGeneratedOutput();
    if (mask.valid) debug.importance_mask = mask.mask;
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
                         *run.frame_debug, debug,
                         (run.hdr_uniforms_ready ? &run.hdr_uniforms : nullptr),
                         run.width, run.height, run.desc.Format);
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

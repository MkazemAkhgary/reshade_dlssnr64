#include "pass_session.h"
#include "profiling/profile.h"

#include "compose/compose.h"
#include "hdr/hdr_state.h"
#include "pipeline_graph.h"

namespace reshade_dlssnr {

// Compose the model output into PassResources::GetCompositeOutput(). HDR uses
// the packed carrier for model-domain values and the original source resource
// for full-precision HDR composition.
PipelineStatus PassSession::StepComposeModelOutput(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepComposeModelOutput");
  PassSession* self = run.session;
  if (run.codec == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }

  // Backbuffer composes the model edit against the backbuffer; PostDLSS composes
  // it against the model input, which is the DLSS output or a snapshot of it.
  const bool hdr_active = run.hdr_uniforms_ready &&
                          run.hdr != nullptr &&
                          run.hdr->ResolvedFormat() != HdrFormat::Sdr;

  // Compare against the actual Grade Model Input result, then apply only the
  // model residual to the image before that grade section.
  const bool apply_edit_to_pre_grade =
      run.input_grade_apply_to_pre_grade && run.grade_input.enabled &&
      run.model_color != nullptr && run.model_output != nullptr;

  ID3D12Resource* original = hdr_active
                                 ? self->resources_.GetPackedOutput()
                                 : (run.stage != nullptr && run.stage->owns_submit
                                        ? run.backbuffer
                                        : run.model_input);

  const model_pipeline::HdrUniforms* hdr_ptr =
      hdr_active ? &run.hdr_uniforms : nullptr;

  // The model output is a real image at every intensity: at zero the evaluation
  // step writes the model input into it, so nothing here special-cases that.
  ID3D12Resource* enhanced_for_compose = run.model_output;

  // The reference is resolved where its image is produced: model-input
  // preparation sets the ungraded one, and the input grade replaces it when the
  // edit is applied to the pre-grade image.
  ID3D12Resource* compose_input = run.compose_reference;
  if (compose_input == nullptr) compose_input = run.model_input;
  if (hdr_active && !apply_edit_to_pre_grade &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      self->resources_.GetUnliftedWorkingInput() != nullptr) {
    compose_input = self->resources_.GetUnliftedWorkingInput();
  }

  // The model input carries the local-exposure lift, and the base does not, so
  // the residual has to supply it: the same ratio of the same two images the
  // normal path measures its lift between. Deriving it here rather than from
  // the gain map is what makes the two modes agree - a separately reconstructed
  // lift is not the lift the model saw.
  ID3D12Resource* lift_input = nullptr;
  ID3D12Resource* lift_enhanced = nullptr;
  if (apply_edit_to_pre_grade &&
      run.hdr_uniforms.local_exposure_strength > 0.0f) {
    if (hdr_active) {
      lift_input = self->resources_.GetUnliftedWorkingInput();
      lift_enhanced = self->resources_.GetWorkingInput();
    } else if (self->local_exposure_.scale_valid) {
      // The lifted texture survives the frame, so only bind it when this
      // frame's lift ran into it.
      lift_input = self->resources_.GetWorkingInput();
      lift_enhanced = self->resources_.GetLiftedWorkingInput();
    }
    if (lift_input == nullptr || lift_enhanced == nullptr) {
      lift_input = nullptr;
      lift_enhanced = nullptr;
    }
  }

  if (!model_pipeline::ComposeOutput(
          self->command_, self->StageList(run, self->command_),
          self->resources_, run.codec, original, run.model_input,
          compose_input, enhanced_for_compose, run.width, run.height,
          run.pass_state.strength, run.pass_state.residual, hdr_ptr,
          apply_edit_to_pre_grade, lift_input, lift_enhanced)) {
    return self->FailStep("failed to compose model output");
  }
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

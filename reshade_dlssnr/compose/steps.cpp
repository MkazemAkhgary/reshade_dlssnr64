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
  ID3D12Resource* original = hdr_active
                                 ? self->resources_.GetPackedOutput()
                                 : (run.stage != nullptr && run.stage->owns_submit
                                        ? run.backbuffer
                                        : run.model_input);

  const model_pipeline::HdrUniforms* hdr_ptr =
      hdr_active ? &run.hdr_uniforms : nullptr;

  // Restore exposure applies to the model output at every intensity, so the
  // composition must see the model output (and its restored scale) even when
  // intensity is zero. Bypass the model output only when no such correction
  // is active.
  const bool restore_exposure_active =
      hdr_active &&
      run.hdr_state.enable &&
      run.hdr_state.local_exposure.enabled &&
      run.hdr_state.local_exposure.restore_exposure &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      run.hdr_state.local_exposure.restore_strength > 0.0f &&
      self->resources_.GetPackedOutput() != nullptr &&
      self->resources_.GetGlobalHdrCarrier() != nullptr;
  const bool bypass_model =
      run.pass_state.intensity <= 0.0f && !restore_exposure_active;

  // Intensity == 0 means the model edit is explicitly disabled. Use the
  // exact model input as the compose reference so NGX/evaluation noise cannot
  // create a residual when the model output is intentionally bypassed.
  ID3D12Resource* enhanced_for_compose =
      bypass_model ? run.model_color : run.model_output;

  ID3D12Resource* compose_input = run.compose_reference;
  if (compose_input == nullptr) compose_input = run.model_input;
  if (hdr_active && !bypass_model &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      self->resources_.GetUnliftedWorkingInput() != nullptr) {
    compose_input = self->resources_.GetUnliftedWorkingInput();
  }

  // Compare against the actual Grade Model Input result, then apply only the
  // model residual to the image before that grade section. The option remains
  // active at zero model intensity: there the comparison endpoints are made
  // identical, while the HDR compose path retains local exposure in its base.
  const bool apply_edit_to_pre_grade =
      run.input_grade_apply_to_pre_grade && run.grade_input.enabled &&
      run.model_color != nullptr && run.model_output != nullptr;
  if (apply_edit_to_pre_grade) {
    compose_input = run.model_color;
  }

  if (!model_pipeline::ComposeOutput(
          self->command_, self->StageList(run, self->command_),
          self->resources_, run.codec, original, run.model_input,
          compose_input, enhanced_for_compose, run.width, run.height,
          run.pass_state.strength, run.pass_state.soft_compression,
          run.pass_state.residual, hdr_ptr, apply_edit_to_pre_grade)) {
    self->FailForStep("failed to compose model output");
    return PipelineStatus::Fail;
  }
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

#include "pipeline/pass_session.h"

#include "grade/grade.h"
#include "hdr/hdr_state.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

int GradeFormatCode(const PassRun& run, ID3D12Resource* target,
                    bool model_sdr_domain) {
  // The model input/output textures hold sRGB-encoded SDR even on the HDR
  // path (the carrier downsampled from the packed HDR split). GradeInPlace
  // therefore decodes/encodes them through the ordinary SDR path, symmetric
  // for both grades.
  if (run.hdr_uniforms_ready &&
      (target == run.model_color || target == run.model_output ||
       model_sdr_domain)) {
    return HdrFormatCode(HdrFormat::Sdr);
  }
  return target != nullptr ? HdrFormatCode(DetectFormatFromDxgi(target->GetDesc().Format))
                           : 0;
}

}  // namespace

PipelineStatus PassSession::GradeInPlace(PassRun& run, ID3D12Resource* target,
                                         const GradeSettings& grade,
                                         uint32_t scratch_slot,
                                         bool model_sdr_domain,
                                         ID3D12Resource* exposure_map) {
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;
  if (target == nullptr) return PipelineStatus::Ok;
  // Runs even when the grade is disabled; unexpose is a separate transform.
  if (!grade.enabled && exposure_map == nullptr) return PipelineStatus::Ok;
  if (run.device == nullptr || run.codec == nullptr) return PipelineStatus::Ok;

  // Unexpose Hunt-effect controls. They only affect the unexpose path,
  // which is part of Local Exposure, so the values live with that
  // layer. Defaults match the shader's own defaults when the settings
  // pointer is unavailable.
  const float unexpose_naturalness = run.settings != nullptr
      ? run.settings->hdr.local_exposure.unexpose_naturalness : 0.2f;
  const float unexpose_reference_l = run.settings != nullptr
      ? run.settings->hdr.local_exposure.unexpose_reference_l : 0.5f;

  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = desc.Height;

  if (!self->resources_.EnsureScratch(run.device, desc, width, height,
                                      scratch_slot)) {
    self->FailForStep("failed to create the grade scratch texture");
    return PipelineStatus::Fail;
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  if (!model_pipeline::ApplyGrade(
          self->command_, list, run.codec, target,
          self->resources_.GetScratch(scratch_slot), width, height,
          GradeFormatCode(run, target, model_sdr_domain), grade,
          exposure_map, unexpose_naturalness, unexpose_reference_l)) {
    self->FailForStep("failed to apply the grade");
    return PipelineStatus::Fail;
  }

  self->command_.Transition(list, target,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

PipelineStatus PassSession::StepGradeModelInput(PassRun& run) {
  if (run.pass == nullptr) return PipelineStatus::Ok;
  PassSession* self = run.session;

  // Observer-only: the Input Grade mutates the model input in place, so
  // snapshot it first for the pre-grade debug view. Gated on the view being
  // selected by the active pass; no cost otherwise.
  if (self != nullptr && run.settings != nullptr && run.model_color != nullptr &&
      DebugViewNeedsPreGradeInput(run.settings->debug_view) &&
      static_cast<int>(self->pass_index_) == run.settings->active_pass) {
    const D3D12_RESOURCE_DESC desc = run.model_color->GetDesc();
    if (self->resources_.EnsureScratch(
            run.device, desc, static_cast<unsigned int>(desc.Width),
            static_cast<unsigned int>(desc.Height),
            kDebugPreGradeInputSlot)) {
      ID3D12Resource* snapshot =
          self->resources_.GetScratch(kDebugPreGradeInputSlot);
      ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
      self->command_.Transition(list, run.model_color,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
      self->command_.Transition(list, snapshot, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(snapshot, run.model_color);
      self->command_.Transition(list, run.model_color,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, snapshot,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  const GradeSettings& grade = run.pass->grade_input;
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_model_input pass=%u luma=%.3f chroma=%.3f",
            run.pass_index, grade.luma, grade.chroma);
  PipelineStatus status =
      GradeInPlace(run, run.model_color, grade, kGradeScratchSlotA);
  if (status != PipelineStatus::Ok) return status;

  const bool use_unlifted_reference =
      run.hdr_uniforms_ready && run.hdr != nullptr &&
      run.hdr->ResolvedFormat() != HdrFormat::Sdr &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      run.session != nullptr &&
      run.session->resources_.GetUnliftedWorkingInput() != nullptr;
  if (use_unlifted_reference) {
    return GradeInPlace(run,
                        run.session->resources_.GetUnliftedWorkingInput(),
                        grade, kGradeScratchSlotA, true);
  }
  return PipelineStatus::Ok;
}

// Runs right after the model output barrier, before anything consumes the
// model output: compose, the importance mask, and the debug views. Grades
// the model output in place, in the model's sRGB-encoded SDR domain —
// symmetric to the Input Grade on the model input — so downstream steps see
// it as if the model had produced the graded SDR.
PipelineStatus PassSession::StepGradeModelOutput(PassRun& run) {
  if (run.pass == nullptr || run.session == nullptr) return PipelineStatus::Ok;
  PassSession* self = run.session;

  // Observer-only: snapshot the ungraded model output for the "raw" debug
  // view before the Output Grade overwrites it. Gated on the view being
  // selected by the active pass; no cost otherwise.
  if (run.settings != nullptr && run.model_output != nullptr &&
      DebugViewNeedsPreGradeOutput(run.settings->debug_view) &&
      static_cast<int>(self->pass_index_) == run.settings->active_pass) {
    const D3D12_RESOURCE_DESC desc = run.model_output->GetDesc();
    if (self->resources_.EnsureScratch(
            run.device, desc, static_cast<unsigned int>(desc.Width),
            static_cast<unsigned int>(desc.Height),
            kDebugPreGradeOutputSlot)) {
      ID3D12Resource* snapshot =
          self->resources_.GetScratch(kDebugPreGradeOutputSlot);
      ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
      self->command_.Transition(list, run.model_output,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
      self->command_.Transition(list, snapshot, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(snapshot, run.model_output);
      self->command_.Transition(list, run.model_output,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, snapshot,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  const GradeSettings& grade = run.pass->grade_output;

  // Unexpose is gated on the local exposure layer having produced a map this
  // frame, and runs whether or not the grade itself is enabled.
  ID3D12Resource* exposure_map = nullptr;
  if (run.settings != nullptr &&
      run.settings->hdr.enable &&
      run.settings->hdr.local_exposure.enabled &&
      run.settings->hdr.local_exposure.unexpose_on_model_output &&
      run.hdr_uniforms_ready && run.hdr != nullptr &&
      run.hdr->ResolvedFormat() != HdrFormat::Sdr &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      self->resources_.GetEffectiveExposureMap() != nullptr) {
    exposure_map = self->resources_.GetEffectiveExposureMap();
  }

  if (!grade.enabled && exposure_map == nullptr) return PipelineStatus::Ok;

  if (grade.enabled) {
    TRACE_EVT(run.session, "grade_model_output pass=%u luma=%.3f chroma=%.3f",
              run.pass_index, grade.luma, grade.chroma);
  }
  if (exposure_map != nullptr) {
    TRACE_EVT(run.session, "unexpose_model_output pass=%u", run.pass_index);
  }
  return GradeInPlace(run, run.model_output, grade, kGradeScratchSlotB, true,
                      exposure_map);
}

}  // namespace reshade_dlssnr

#include "pipeline/pass_session.h"

#include "grade/grade.h"
#include "hdr/hdr_state.h"
#include "pipeline/pipeline_graph.h"
#include "pipeline/pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

HdrFormat GradeFormat(const PassRun& run, ID3D12Resource* target) {
  if (run.hdr_uniforms_ready && target == run.model_color) {
    return HdrFormat::Sdr;
  }
  return target != nullptr ? DetectFormatFromDxgi(target->GetDesc().Format)
                           : HdrFormat::Sdr;
}

}  // namespace

PipelineStatus PassSession::GradeInPlace(PassRun& run, ID3D12Resource* target,
                                         const GradeSettings& grade,
                                         uint32_t scratch_slot) {
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;
  if (!grade.enabled || target == nullptr) return PipelineStatus::Ok;
  if (run.device == nullptr || run.codec == nullptr) return PipelineStatus::Ok;

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
          GradeFormat(run, target), grade)) {
    self->FailForStep("failed to apply the grade");
    return PipelineStatus::Fail;
  }

  self->command_.Transition(list, target,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

PipelineStatus PassSession::StepGradeModelInput(PassRun& run) {
  if (run.pass == nullptr) return PipelineStatus::Ok;
  const GradeSettings& grade = run.pass->grade_input;
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_model_input pass=%u luma=%.3f chroma=%.3f",
            run.pass_index, grade.luma, grade.chroma);
  return GradeInPlace(run, run.model_color, grade, kGradeScratchSlotA);
}

PipelineStatus PassSession::StepGradeModelOutput(PassRun& run) {
  if (run.pass == nullptr || run.session == nullptr) return PipelineStatus::Ok;
  const GradeSettings& grade = run.pass->grade_output;
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_model_output pass=%u luma=%.3f chroma=%.3f",
            run.pass_index, grade.luma, grade.chroma);
  return GradeInPlace(run, run.session->resources_.GetCompositeOutput(),
                      grade, kGradeScratchSlotB);
}

}  // namespace reshade_dlssnr

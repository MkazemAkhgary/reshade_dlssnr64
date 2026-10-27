#include "pipeline/pass_session.h"
#include "profiling/profile.h"

#include <algorithm>

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

PipelineStatus PassSession::GradeInPlace(PassRun& run, ID3D12Resource* source,
                                         ID3D12Resource* target,
                                         const GradeSettings& grade,
                                         uint32_t scratch_slot,
                                         bool model_sdr_domain,
                                         ID3D12Resource* local_carrier,
                                         ID3D12Resource* global_carrier,
                                         ID3D12Resource* global_scale,
                                         float restore_mode,
                                         float restore_strength) {
  DLSSNR_PROFILE_RENDER_L3("GradeInPlace");
  PassSession* self = run.session;
  if (self == nullptr) return PipelineStatus::Ok;
  if (source == nullptr || target == nullptr) return PipelineStatus::Ok;
  // Runs even when the grade is disabled; exposure restoration is separate.
  const bool restoring = local_carrier != nullptr &&
                         global_carrier != nullptr;
  if (!grade.enabled && !restoring) return PipelineStatus::Ok;
  if (run.device == nullptr || run.codec == nullptr) return PipelineStatus::Ok;

  const D3D12_RESOURCE_DESC desc = target->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = desc.Height;

  if (!self->resources_.EnsureScratch(run.device, desc, width, height,
                                      scratch_slot)) {
    return self->FailStep("failed to create the grade scratch texture");
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  if (!model_pipeline::ApplyGrade(
          self->command_, list, run.codec, source, target,
          self->resources_.GetScratch(scratch_slot), width, height,
          GradeFormatCode(run, target, model_sdr_domain), grade,
          local_carrier, global_carrier, global_scale, restore_mode, restore_strength)) {
    return self->FailStep("failed to apply the grade");
  }

  if (source != target) {
    self->command_.Transition(list, source,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  self->command_.Transition(list, target,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  return PipelineStatus::Ok;
}

PipelineStatus PassSession::StepGradeModelInput(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepGradeModelInput");
  if (!run.pass_bound) return PipelineStatus::Ok;
  PassSession* self = run.session;

  const GradeSettings& grade = run.grade_input;
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_model_input pass=%u luma=%.3f chroma=%.3f",
            self->pass_id_, grade.luma, grade.chroma);

  ID3D12Resource* graded = self->resources_.GetGradedWorkingInput();
  if (graded == nullptr) {
    return self->FailStep("graded model input buffer is unavailable");
  }

  PipelineStatus status = GradeInPlace(
      run, run.model_color, graded, grade, kGradeScratchSlotA,
      /*model_sdr_domain=*/true);
  if (status != PipelineStatus::Ok) return status;

  run.model_color = graded;
  return PipelineStatus::Ok;
}

// Runs right after the model output barrier, before anything consumes the
// model output: compose, the exposure mask, and the debug views. Grades
// the model output in place, in the model's sRGB-encoded SDR domain -
// symmetric to the Input Grade on the model input - so downstream steps see
// it as if the model had produced the graded SDR.
PipelineStatus PassSession::StepGradeModelOutput(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepGradeModelOutput");
  if (!run.pass_bound || run.session == nullptr) return PipelineStatus::Ok;
  PassSession* self = run.session;

  // Keep the synthetic guide history after the current model evaluation has
  // consumed the generated vector field. Update only after a successful model
  // result so a failed frame cannot become the next motion reference.
  if (run.synthetic_motion_ready && run.eval_result == NVSDK_NGX_Result_Success &&
      run.resources != nullptr && run.model_color != nullptr) {
    ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
    ID3D12Resource* previous_input = run.resources->GetSyntheticPreviousInput();
    ID3D12Resource* previous_depth = run.resources->GetSyntheticPreviousDepth();
    ID3D12Resource* current_depth = run.resources->GetSyntheticCurrentDepth();
    if (list != nullptr && previous_input != nullptr && previous_depth != nullptr &&
        current_depth != nullptr) {
      const D3D12_RESOURCE_STATES input_state =
          self->command_.GetResourceState(run.model_color);
      self->command_.CopyWithTransitions(
          list, run.model_color,
          input_state == D3D12_RESOURCE_STATE_COMMON
              ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
              : input_state,
          previous_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

      self->command_.CopyWithTransitions(
          list, current_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          previous_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->temporal_.synthetic_history_valid = true;
      self->temporal_.synthetic_history_frame = run.frame_id;
    } else {
      self->temporal_.synthetic_history_valid = false;
      self->temporal_.synthetic_history_frame = 0;
    }
  }

  // Observer-only: snapshot the ungraded model output for the "raw" debug
  // view before the Output Grade overwrites it. Gated on the view being
  // selected by the active pass; no cost otherwise.
  if (run.settings != nullptr && run.model_output != nullptr &&
      DebugViewNeedsPreGradeOutput(run.debug_view) &&
      self->pass_id_ == run.settings->active_pass_id) {
    const D3D12_RESOURCE_DESC desc = run.model_output->GetDesc();
    if (self->resources_.EnsureScratch(
            run.device, desc, static_cast<unsigned int>(desc.Width),
            static_cast<unsigned int>(desc.Height),
            kDebugPreGradeOutputSlot)) {
      ID3D12Resource* snapshot =
          self->resources_.GetScratch(kDebugPreGradeOutputSlot);
      ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
      self->command_.CopyWithTransitions(
          list, run.model_output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          snapshot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  const GradeSettings& grade = run.grade_output;

  // Restore model exposure after the model has reconstructed local shading.
  // The local multiplier is read per pixel from the carrier pair. Only the
  // global endpoint needs a reduction; there is no blurred tile map.
  ID3D12Resource* local_carrier = nullptr;
  ID3D12Resource* global_carrier = nullptr;
  ID3D12Resource* global_scale = nullptr;
  const LocalExposureLayerSettings& local = run.hdr_state.local_exposure;
  const float restore_mode = std::clamp(local.restore_mode, 0.0f, 1.0f);
  const float restore_strength = std::clamp(local.restore_strength, 0.0f, 1.0f);
  if (run.hdr_state.enable && local.enabled && local.restore_exposure &&
      run.hdr_uniforms_ready && run.hdr != nullptr &&
      run.hdr->ResolvedFormat() != HdrFormat::Sdr &&
      run.hdr_uniforms.local_exposure_strength > 0.0f &&
      restore_strength > 0.0f &&
      self->resources_.GetPackedOutput() != nullptr &&
      self->resources_.GetGlobalHdrCarrier() != nullptr) {
    local_carrier = self->resources_.GetPackedOutput();
    global_carrier = self->resources_.GetGlobalHdrCarrier();

    if (restore_mode > 0.0f) {
      if (!self->resources_.EnsureLocalExposureRestoreResources(
              run.device, run.width, run.height)) {
        return self->FailStep("failed to create local exposure restore resources");
      }

      ID3D12Resource* tile_log_scales =
          self->resources_.GetLocalExposurePartialSums();
      global_scale = self->resources_.GetLocalExposureGlobalScale();
      ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
      ComputeConstants reduce_constants = {};
      reduce_constants.Width = run.width;
      reduce_constants.Height = run.height;
      ID3D12Resource* reduce_srvs[kSrvCount] = {
          local_carrier, global_carrier, local_carrier, global_carrier};
      ID3D12Resource* reduce_uavs[kUavCount] = {tile_log_scales};
      self->command_.Transition(list, local_carrier,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, global_carrier,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, tile_log_scales,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (run.codec == nullptr || !run.codec->Dispatch(
              "ReduceLocalExposureTiles", list, reduce_constants,
              reduce_srvs, reduce_uavs)) {
        return self->FailStep("failed to reduce local exposure multipliers");
      }
      self->command_.UavBarrier(list, tile_log_scales);
      self->command_.Transition(list, tile_log_scales,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

      ComputeConstants global_constants = {};
      global_constants.Width = self->resources_.LocalExposurePartialWidth();
      global_constants.Height = self->resources_.LocalExposurePartialHeight();
      global_constants.shader.Params0.x = static_cast<float>(run.width);
      global_constants.shader.Params0.y = static_cast<float>(run.height);
      ID3D12Resource* global_srvs[kSrvCount] = {
          tile_log_scales, tile_log_scales, tile_log_scales, tile_log_scales};
      ID3D12Resource* global_uavs[kUavCount] = {global_scale};
      self->command_.Transition(list, global_scale,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      if (!run.codec->Dispatch("ReduceLocalExposureGlobal", list,
                               global_constants, global_srvs, global_uavs,
                               1, 1, 1)) {
        return self->FailStep("failed to compute global local-exposure scale");
      }
      self->command_.UavBarrier(list, global_scale);
      self->command_.Transition(list, global_scale,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  if (!grade.enabled && local_carrier == nullptr) return PipelineStatus::Ok;

  if (grade.enabled) {
    TRACE_EVT(run.session, "grade_model_output pass=%u luma=%.3f chroma=%.3f",
              self->pass_id_, grade.luma, grade.chroma);
  }
  if (local_carrier != nullptr) {
    TRACE_EVT(run.session, "restore_model_exposure pass=%u mode=%.3f strength=%.3f",
              self->pass_id_, restore_mode, restore_strength);
  }
  return GradeInPlace(run, run.model_output, run.model_output, grade,
                      kGradeScratchSlotB, true,
                      local_carrier, global_carrier, global_scale,
                      restore_mode, restore_strength);
}

}  // namespace reshade_dlssnr

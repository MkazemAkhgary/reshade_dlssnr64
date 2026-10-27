#include "pass_session.h"
#include "profiling/profile.h"

#include <cstdint>
#include <cstdio>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "dlss_guides.h"
#include "exposure_mask/exposure_mask.h"
#include "pass_resources.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"
#include "model/model_input.h"
#include "model/model_pipeline.h"

namespace reshade_dlssnr {

namespace {

// Weight of the fresh model edit when it is accumulated into the stored one.
// 1.0 makes every refresh a step; lower converges over several refreshes.
constexpr float kFreshEditBlend = 0.5f;

}  // namespace

// Select the image the model consumes. Backbuffer reads the swapchain image;
// PreDLSS reads the game's Color input; PostDLSS reads the captured DLSS
// output. Chained game-resource passes snapshot the previous result.
PipelineStatus PassSession::StepResolveModelInput(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepResolveModelInput");
  PassSession* self = run.session;
  if (run.stage != nullptr && run.stage->owns_submit) {
    run.model_input = run.backbuffer;
    if (run.model_input != nullptr) {
      self->command_.Transition(self->command_.GetCommandList(), run.model_input,
                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return PipelineStatus::Ok;
  }

  if (run.stage != nullptr && run.stage->is_pre_dlss) {
    if (run.output_resource == nullptr) return PipelineStatus::SkipFrame;
    run.model_input = run.output_resource;
    return PipelineStatus::Ok;
  }

  // PostDLSS: first pass reads either the game output or its optional clone.
  if (run.is_first_pass) {
    if (run.guides.dlss_output_color == nullptr) {
      TRACE_EVT(self, "SKIP stage=PostDLSS pass=%u reason=no_guide_color",
                self->pass_id_);
      return PipelineStatus::SkipFrame;
    }
    run.model_input = run.guides.dlss_output_color;
    // A cloned output is known to be an SRV. In direct mode this aliases the
    // game's output_resource, whose state was seeded from its descriptor in
    // StepSeedResourceStates; preserve that state instead of overwriting it.
    if (run.guides.clone_resources) {
      self->command_.SetResourceState(
          run.model_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    return PipelineStatus::Ok;
  }

  // Chained pass: snapshot the previous result into scratch slot 0.
  if (run.device == nullptr) return PipelineStatus::SkipFrame;
  constexpr uint32_t kSnapshotSlot = kInputSnapshotSlot;
  if (!self->resources_.EnsureScratch(run.device, run.desc, run.width, run.height,
                                kSnapshotSlot)) {
    self->FailForStep("failed to create input snapshot");
    TRACE_EVT(self, "FAIL pass=%u reason=ensure_scratch", self->pass_id_);
    return PipelineStatus::Fail;
  }
  ID3D12Resource* scratch = self->resources_.GetScratch(kSnapshotSlot);
  TRACE_CMD(self, "snapshot_input scratch=%d from=%d",
            trace::ResourceId(scratch), trace::ResourceId(run.output_resource));
  self->command_.Transition(run.list, run.output_resource,
                      D3D12_RESOURCE_STATE_COPY_SOURCE);
  self->command_.Transition(run.list, scratch, D3D12_RESOURCE_STATE_COPY_DEST);
  run.list->CopyResource(scratch, run.output_resource);
  self->command_.Transition(run.list, scratch,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  run.model_input = scratch;
  return PipelineStatus::Ok;
}
// Build the full-resolution combined local-exposure map used by HDR inverse compose. Working
// scale only affects the per-frame evaluation size, which FillEvaluateParams
// sets on every frame, so changing working scale never triggers a rebuild. A
// rebuild happens only when the swapchain resolution itself changes.
PipelineStatus PassSession::StepEnsureFeature(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepEnsureFeature");
  PassSession* self = run.session;
  if (run.runtime == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }

  // The list this stage records on: its own for Backbuffer, the game's for
  // PreDLSS and PostDLSS.
  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);

  // Brackets the call so a crash inside it is attributable rather than just
  // "somewhere in this step".
  TRACE_EVT(self, "ensure_feature stage=%s pass=%u list=%p %ux%u",
            run.stage != nullptr ? run.stage->name : "?", self->pass_id_,
            static_cast<const void*>(list), run.width, run.height);

  if (!self->feature_.EnsureFeature(*run.runtime, run.pass_state, list,
                                    run.width, run.height)) {
    self->FailForStep(self->feature_.GetLastError());
    if (run.stage != nullptr && run.stage->owns_submit) {
      // Backbuffer owns the list, so it has to close it; game-resource stages
      // must leave the game's list alone.
      list->Close();
    }
    return PipelineStatus::Fail;
  }

  if (self->feature_.ConsumeJustRebuilt()) {
    // The feature was just created or rebuilt; skip evaluation this frame so
    // the model has a frame to fully initialize.
    TRACE_EVT(self, "SKIP stage=%s pass=%u reason=feature_rebuilt",
              run.stage != nullptr ? run.stage->name : "?", self->pass_id_);
    if (run.stage != nullptr && run.stage->owns_submit) {
      self->SubmitBackbuffer();
    }
    reshade::log::message(
        reshade::log::level::info,
        "Direct DLSSNR feature created/rebuilt; skipping evaluation this frame");
    return PipelineStatus::SkipFrame;
  }
  return PipelineStatus::Ok;
}
// Downsample the already-split linear SDR base to working resolution with
// exact pixel-footprint box/area averaging, and
// report the resolution the model will actually run at. Gain-map splitting is
// a distinct step so the packed HDR representation is also available to the
// debug and compose paths at full resolution.
PipelineStatus PassSession::StepPrepareModelInput(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepPrepareModelInput");
  PassSession* self = run.session;
  if (run.codec == nullptr) return PipelineStatus::SkipFrame;

  const bool hdr_source = run.hdr_uniforms_ready && run.hdr != nullptr &&
                          run.hdr->ResolvedFormat() != HdrFormat::Sdr;
  ID3D12Resource* model_input = run.stage != nullptr && run.stage->owns_submit
                                    ? run.backbuffer
                                    : (hdr_source ? self->resources_.GetPackedOutput()
                                                  : run.model_input);
  ID3D12Resource* model_color = model_input;
  ID3D12Resource* model_output = self->resources_.GetOutput();
  unsigned int eval_width = run.width;
  unsigned int eval_height = run.height;

  if (!model_pipeline::PrepareInput(
          self->command_, self->StageList(run, self->command_),
          self->resources_, run.codec, model_input,
          &model_color, &model_output, &eval_width, &eval_height,
          run.work_width, run.work_height,
          run.hdr_uniforms_ready && run.hdr != nullptr &&
              run.hdr->ResolvedFormat() != HdrFormat::Sdr,
          run.pass_bound && !run.grade_input.enabled)) {
    return self->FailStep("failed to prepare model input");
  }
  run.compose_reference = model_color;

  if (hdr_source && run.hdr_uniforms.local_exposure_strength > 0.0f) {
    ID3D12Resource* global_carrier = self->resources_.GetGlobalHdrCarrier();
    D3D12_RESOURCE_DESC reference_desc =
        self->resources_.GetWorkingInput()->GetDesc();
    if (global_carrier == nullptr ||
        !self->resources_.EnsureUnliftedWorkingInput(
            run.device, reference_desc, run.work_width, run.work_height)) {
      return self->FailStep("failed to create unlifted HDR composition reference");
    }

    ID3D12Resource* unlifted_input =
        self->resources_.GetUnliftedWorkingInput();
    ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
    self->command_.SetResourceState(unlifted_input,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    self->command_.Transition(list, global_carrier,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, unlifted_input,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ComputeConstants reference_constants = {};
    reference_constants.Width = run.work_width;
    reference_constants.Height = run.work_height;
    reference_constants.shader.Params0.w = 1.0f;
    ID3D12Resource* reference_srvs[kSrvCount] = {
        global_carrier, global_carrier, global_carrier, global_carrier};
    ID3D12Resource* reference_uavs[kUavCount] = {unlifted_input};
    if (!run.codec->Dispatch("PrepareModelInput", list, reference_constants,
                             reference_srvs, reference_uavs)) {
      return self->FailStep("failed to prepare unlifted HDR composition reference");
    }
    self->command_.UavBarrier(list, unlifted_input);

    // Apply-to-pre-grade composes onto the pre-grade image, so it has to carry
    // the local lift itself. Build that lift here, at working resolution, and
    // reuse the unlifted buffer for it: nothing else reads that buffer in this
    // mode. Compose reconstructs it with the joint-bilateral filter so the base
    // keeps its full-resolution detail.
    ID3D12Resource* packed = self->resources_.GetPackedOutput();
    if (run.input_grade_apply_to_pre_grade && run.grade_input.enabled &&
        packed != nullptr) {
      self->command_.Transition(list, unlifted_input,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ComputeConstants gain_constants = {};
      gain_constants.Width = run.work_width;
      gain_constants.Height = run.work_height;
      gain_constants.shader.Params0.z = 1.0f;
      ID3D12Resource* gain_srvs[kSrvCount] = {
          packed, global_carrier, packed, packed};
      ID3D12Resource* gain_uavs[kUavCount] = {unlifted_input};
      if (!run.codec->Dispatch("PrepareModelInput", list, gain_constants,
                               gain_srvs, gain_uavs)) {
        return self->FailStep("failed to build the local-exposure gain");
      }
      self->command_.UavBarrier(list, unlifted_input);
    }

    self->command_.Transition(list, unlifted_input,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }

  run.model_color = model_color;
  run.model_output = model_output;
  run.eval_width = eval_width;
  run.eval_height = eval_height;
  return PipelineStatus::Ok;
}
// Fill the real NGX parameter block with tuning values, resources and subrects
// for this evaluation.
PipelineStatus PassSession::StepFillEvaluateParams(PassRun& run) {
  DLSSNR_PROFILE_CORE_L2("StepFillEvaluateParams");
  PassSession* self = run.session;
  if (run.temporal_reuse) return PipelineStatus::Ok;
  if (run.runtime == nullptr || !run.pass_bound) {
    return PipelineStatus::SkipFrame;
  }
  if (!self->FillEvaluateParams(*run.runtime, run.pass_state, run.guides,
                                run.model_color, run.model_output,
                                run.eval_width, run.eval_height, run.width,
                                run.height)) {
    return self->FailStep(self->reason_);
  }
  return PipelineStatus::Ok;
}
// Builds one line describing exactly what NGX is being asked to do.
// EvaluateFeature rejects on a format or dimension mismatch with no other
// clue, and the two stages feed it different resources (a swapchain backbuffer
// for Present, the game's DLSS output resource for PostDLSS), so recording the
// concrete descriptors is what makes a failure attributable to one of them.
//
// Logged through the ReShade log, not the trace: the trace is opt-in via
// DLSSNR_TRACE, and this has to be visible to someone who just enabled the
// pass and saw it fail.
//
// The caller emits it on first use and when the descriptor contract changes,
// plus always when a resource is null or an evaluation fails. Resource pointer
// identity is intentionally not treated as a contract change: the backbuffer
// rotates through swapchain images and makes that pointer change every frame.
// Logging those expected rotations caused measurable Present-path overhead
// without adding diagnostic information.
void PassSession::FormatEvalLine(const PassRun& run, const EvalLogKey& key,
                                 char* line, size_t line_size) {
  char color_part[128] = "color=null";
  char out_part[128] = "out=null";
  if (key.color != nullptr) {
    snprintf(color_part, sizeof(color_part), "color=%ux%u/%s", key.color_width,
             key.color_height,
             FormatName(static_cast<DXGI_FORMAT>(key.color_format)));
  }
  if (key.output != nullptr) {
    snprintf(out_part, sizeof(out_part), "out=%ux%u/%s", key.output_width,
             key.output_height,
             FormatName(static_cast<DXGI_FORMAT>(key.output_format)));
  }
  snprintf(line, line_size,
           "DLSSNR evaluate st=%s %s %s eval=%ux%u frame=%ux%u "
           "in=%ux%u work=%ux%u guides=%d pass=%u hdl=%p params=%p",
           run.stage != nullptr ? run.stage->name : "?", color_part, out_part,
           key.eval_width, key.eval_height, key.frame_width, key.frame_height,
           key.input_width, key.input_height, key.work_width, key.work_height,
           key.guides_valid, key.pass_index, key.feature_handle, key.params);
}
// Fills `key` from the current run. Split out so the comparison and the
// formatting read the same values.
void PassSession::BuildEvalKey(const PassRun& run, void* feature_handle,
                               EvalLogKey* key) {
  *key = EvalLogKey{};
  key->stage = run.stage != nullptr ? run.stage->name : "?";
  key->color = run.model_color;
  key->output = run.model_output;
  if (run.model_color != nullptr) {
    const D3D12_RESOURCE_DESC d = run.model_color->GetDesc();
    key->color_width = static_cast<unsigned int>(d.Width);
    key->color_height = static_cast<unsigned int>(d.Height);
    key->color_format = static_cast<int>(d.Format);
  }
  if (run.model_output != nullptr) {
    const D3D12_RESOURCE_DESC d = run.model_output->GetDesc();
    key->output_width = static_cast<unsigned int>(d.Width);
    key->output_height = static_cast<unsigned int>(d.Height);
    key->output_format = static_cast<int>(d.Format);
  }
  key->eval_width = run.eval_width;
  key->eval_height = run.eval_height;
  key->frame_width = run.width;
  key->frame_height = run.height;
  key->input_width = static_cast<unsigned int>(run.desc.Width);
  key->input_height = static_cast<unsigned int>(run.desc.Height);
  key->work_width = run.work_width;
  key->work_height = run.work_height;
  key->guides_valid = run.guides_valid ? 1 : 0;
  key->pass_index = run.pass_id;
  key->feature_handle = feature_handle;
  key->params = run.runtime != nullptr
                    ? static_cast<void*>(run.runtime->Parameters().Get())
                    : nullptr;
}
bool PassSession::EvalKeysDiffer(const EvalLogKey& a, const EvalLogKey& b) {
  // This is a per-session diagnostic, not a per-evaluation trace. Only compare
  // stable descriptor-contract values. Swapchain resource identity naturally
  // rotates, guide availability can fluctuate while the game refreshes its
  // capture, and NGX can change/recreate handles independently of the resource
  // descriptor. None of those warrants reprinting the full INFO line every
  // frame. Feature rebuilds and failed evaluations have their own diagnostics.
  const std::string a_stage = CStrToString(a.stage);
  const std::string b_stage = CStrToString(b.stage);
  return a_stage != b_stage ||
         ((a.color == nullptr) != (b.color == nullptr)) ||
         ((a.output == nullptr) != (b.output == nullptr)) ||
         a.color_width != b.color_width || a.color_height != b.color_height ||
         a.output_width != b.output_width ||
         a.output_height != b.output_height ||
         a.color_format != b.color_format || a.output_format != b.output_format ||
         a.eval_width != b.eval_width || a.eval_height != b.eval_height ||
         a.frame_width != b.frame_width || a.frame_height != b.frame_height ||
         a.input_width != b.input_width || a.input_height != b.input_height ||
         a.work_width != b.work_width || a.work_height != b.work_height;
}
// Invoke NGX.
//
// Backbuffer captures the result and reports a failure after the frame has been
// submitted (see StepSubmitBackbuffer); PostDLSS ignores the result entirely.
PipelineStatus PassSession::StepEvaluateFeature(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepEvaluateFeature");
  PassSession* self = run.session;
  if (run.temporal_reuse) {
    run.eval_result = NVSDK_NGX_Result_Success;
    TRACE_EVT(self, "temporal skip NGX pass=%u", self->pass_id_);
    return PipelineStatus::Ok;
  }
  if (run.runtime == nullptr) return PipelineStatus::SkipFrame;

  void* feature_handle = static_cast<void*>(self->feature_.FeatureHandle());
  EvalLogKey key = {};
  BuildEvalKey(run, feature_handle, &key);

  // Log on first use and on a descriptor/nullness transition. A persistent
  // null input is reported once as a diagnostic, not once per frame; the
  // evaluation failure path carries the actionable error if it persists.
  const bool first = !self->eval_log_key_valid_;
  const bool changed = first || EvalKeysDiffer(key, self->eval_log_key_);

  self->eval_log_written_this_frame_ = false;
  if (changed) {
    char line[384] = {};
    FormatEvalLine(run, key, line, sizeof(line));
    reshade::log::message(reshade::log::level::info, line);
    TRACE_EVT(self, "%s", line);
    self->eval_log_written_this_frame_ = true;
  }

  // Keep the latest pointers as well as the latest descriptor values even
  // when the diagnostic is suppressed. If NGX fails later in this same frame,
  // the error path can report the current evaluation rather than a stale
  // swapchain resource from a previous frame.
  self->eval_log_key_ = key;
  self->eval_log_key_valid_ = true;

  run.eval_result = run.runtime->EvaluateFeature(
      self->StageList(run, self->command_), self->feature_.FeatureHandle(),
      run.runtime->Parameters().Get(), nullptr);
  run.codec->InvalidateBindings();
  return PipelineStatus::Ok;
}
// NGX writes to the model output through a UAV. A state transition alone does
// not guarantee those writes are visible to the subsequent reads, so insert an
// explicit UAV barrier before composition reads it back.
PipelineStatus PassSession::StepModelOutputBarrier(PassRun& run) {
  DLSSNR_PROFILE_RESOURCE_L2("StepModelOutputBarrier");
  PassSession* self = run.session;
  if (run.model_output == nullptr) return PipelineStatus::Ok;
  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  self->command_.UavBarrier(list, run.model_output);

  if (!run.temporal_active) return PipelineStatus::Ok;
  if (run.temporal_reuse) {
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // A repeated evaluation of the same frame does not advance time, so the
  // stored history still describes this content. Keep it and skip the
  // duplicate store: invalidating here leaves the next frame with no history,
  // which is a visible gap in every temporal debug view.
  if (!run.temporal_store_history) {
    return PipelineStatus::Ok;
  }
  if (run.guides.depth == nullptr || run.guides.motion == nullptr ||
      (!run.temporal_reuse && run.eval_result != NVSDK_NGX_Result_Success)) {
    self->temporal_.history_valid = false;
    return PipelineStatus::Ok;
  }

  ID3D12Resource* previous_input = self->resources_.GetTemporalPreviousInput();
  ID3D12Resource* previous_edit = self->resources_.GetTemporalPreviousEdit();
  ID3D12Resource* previous_depth = self->resources_.GetTemporalPreviousDepth();
  ID3D12Resource* validated = self->resources_.GetTemporalValidatedCandidate();
  if (previous_input == nullptr || previous_edit == nullptr ||
      previous_depth == nullptr || validated == nullptr || run.model_color == nullptr ||
      run.codec == nullptr) {
    self->temporal_.history_valid = false;
    return PipelineStatus::Ok;
  }

  ComputeConstants constants = {};
  constants.Width = run.work_width;
  constants.Height = run.work_height;
  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.model_output,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  if (run.temporal_reuse) {
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  self->command_.Transition(list, run.guides.depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_edit,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, previous_depth,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  constants.temporal.GuideBaseSize.x =
      static_cast<float>(run.guides.guide_base_x);
  constants.temporal.GuideBaseSize.y =
      static_cast<float>(run.guides.guide_base_y);
  constants.temporal.GuideBaseSize.z =
      static_cast<float>(run.guides.guide_subrect_width);
  constants.temporal.GuideBaseSize.w =
      static_cast<float>(run.guides.guide_subrect_height);
  constants.temporal.HistoryControl.x = run.temporal_reuse ? 1.0f : 0.0f;
  constants.temporal.HistoryControl.y = kFreshEditBlend;
  ID3D12Resource* history_srvs[kSrvCount] = {
      run.model_color, run.model_output, validated, run.guides.depth};
  ID3D12Resource* history_uavs[kUavCount] = {previous_edit, previous_depth};
  if (!run.codec->Dispatch("TemporalCommitHistory", list, constants,
                           history_srvs, history_uavs)) {
    self->temporal_.history_valid = false;
    return PipelineStatus::Ok;
  }
  self->command_.UavBarrier(list, previous_edit);
  self->command_.UavBarrier(list, previous_depth);

  // Build the output from the stored edit on refresh frames too. Otherwise a
  // refresh frame shows the raw model output while reuse frames show
  // input * exp(edit), and that difference is the break at every refresh.
  if (!run.temporal_reuse) {
    self->command_.Transition(list, previous_edit,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource* apply_srvs[kSrvCount] = {
        run.model_color, previous_edit, previous_edit, previous_edit};
    ID3D12Resource* apply_uavs[kUavCount] = {run.model_output};
    if (run.codec->Dispatch("TemporalApplyEdit", list, constants,
                            apply_srvs, apply_uavs)) {
      self->command_.UavBarrier(list, run.model_output);
    }
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, previous_edit,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  // Advance the photometric reference on every processed frame. The source is
  // exactly the input seen by NGX (after preparation and input grade).
  const D3D12_RESOURCE_STATES input_state =
      self->command_.GetResourceState(run.model_color);
  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(previous_input, run.model_color);
  self->command_.Transition(list, run.model_color,
                            input_state == D3D12_RESOURCE_STATE_COMMON
                                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                : input_state);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  self->temporal_.history_valid = true;
  if (!run.temporal_reuse) {
    self->temporal_.last_refresh_frame = run.frame_id;
  }
  TRACE_EVT(self, "temporal history committed pass=%u reuse=%d frame=%llu",
            self->pass_id_, run.temporal_reuse ? 1 : 0,
            static_cast<unsigned long long>(run.frame_id));
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

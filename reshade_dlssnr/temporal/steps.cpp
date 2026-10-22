#include "pass_session.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

#include "dlss_guides.h"
#include "gpu_readback.h"
#include "pass_resources.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

template <typename T>
void HashTemporalValue(uint64_t* hash, const T& value) {
  static_assert(std::is_trivially_copyable_v<T>);
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&value);
  for (size_t i = 0; i < sizeof(T); ++i) {
    *hash ^= bytes[i];
    *hash *= 1099511628211ull;
  }
}

uint64_t TemporalSettingsSignature(const PassRun& run) {
  uint64_t hash = 1469598103934665603ull;
  const auto& p = run.pass_state;
  HashTemporalValue(&hash, run.width);
  HashTemporalValue(&hash, run.height);
  HashTemporalValue(&hash, run.work_width);
  HashTemporalValue(&hash, run.work_height);
  HashTemporalValue(&hash, p.target);
  HashTemporalValue(&hash, p.preset);
  HashTemporalValue(&hash, p.working_scale);
  HashTemporalValue(&hash, p.intensity);
  HashTemporalValue(&hash, p.temporal_photometric_abs_threshold);
  HashTemporalValue(&hash, p.temporal_photometric_rel_threshold);
  HashTemporalValue(&hash, p.temporal_depth_abs_threshold);
  HashTemporalValue(&hash, p.temporal_depth_rel_threshold);
  HashTemporalValue(&hash, p.temporal_max_displacement);
  HashTemporalValue(&hash, p.temporal_max_invalid_percent);
  HashTemporalValue(&hash, p.temporal_fp16_history);
  HashTemporalValue(&hash, p.synthetic_motion_guides);
  HashTemporalValue(&hash, p.local_tone);
  HashTemporalValue(&hash, p.local_structure);
  HashTemporalValue(&hash, p.skin_structure);
  HashTemporalValue(&hash, p.auto_mask);
  HashTemporalValue(&hash, p.ui_correction);
  const auto& g = run.grade_input;
  HashTemporalValue(&hash, g.enabled);
  HashTemporalValue(&hash, g.luma);
  HashTemporalValue(&hash, g.chroma);
  HashTemporalValue(&hash, g.luma_mode);
  HashTemporalValue(&hash, g.chroma_mode);
  HashTemporalValue(&hash, g.black_level);
  HashTemporalValue(&hash, g.white_level);
  HashTemporalValue(&hash, g.contrast);
  HashTemporalValue(&hash, g.shadows);
  HashTemporalValue(&hash, g.highlights);
  // Only include stable HDR interpretation parameters. Frame-varying exposure
  // measurements are deliberately excluded so adaptation does not reset history.
  HashTemporalValue(&hash, run.hdr_state.format);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k1);
  HashTemporalValue(&hash, run.hdr_state.bt2446_k3);
  HashTemporalValue(&hash, run.hdr_state.bt2446_ip);
  HashTemporalValue(&hash, run.hdr_state.bt2446_crosstalk);
  HashTemporalValue(&hash, run.hdr_uniforms.format);
  HashTemporalValue(&hash, run.hdr_uniforms.local_exposure_strength);
  HashTemporalValue(&hash, run.hdr_uniforms.local_max_brighten);
  HashTemporalValue(&hash, run.hdr_uniforms.local_max_darken);
  HashTemporalValue(&hash, run.hdr_uniforms.midtone);
  HashTemporalValue(&hash, run.hdr_uniforms.local_shadow_strength);
  HashTemporalValue(&hash, run.hdr_uniforms.local_highlight_strength);
  HashTemporalValue(&hash, run.hdr_uniforms.local_shadow_range);
  HashTemporalValue(&hash, run.hdr_uniforms.local_highlight_range);
  HashTemporalValue(&hash, run.hdr_uniforms.local_highlight_protection);
  HashTemporalValue(&hash, run.hdr_uniforms.local_shadow_protection);
  HashTemporalValue(&hash, run.hdr_uniforms.local_detail_retention);
  HashTemporalValue(&hash, run.hdr_uniforms.local_edge_protection);
  HashTemporalValue(&hash, run.hdr_uniforms.local_radius_px);
  HashTemporalValue(&hash, run.hdr_uniforms.local_samples);
  HashTemporalValue(&hash, run.fp16_storage);
  return hash;
}

float TemporalReadInvalidFraction(GpuReadback& readback, ID3D12Fence* fence,
                                  unsigned int width, unsigned int height,
                                  bool* out_valid) {
  if (out_valid != nullptr) *out_valid = false;
  uint32_t bytes = 0;
  const void* data = readback.TryGet(fence, &bytes);
  if (data == nullptr || bytes < sizeof(uint32_t) * 2 || width == 0 || height == 0)
    return 1.0f;
  const auto* values = static_cast<const uint32_t*>(data);
  if (out_valid != nullptr) *out_valid = values[1] != 0;
  return std::min(1.0f, static_cast<float>(values[0]) /
                           static_cast<float>(static_cast<uint64_t>(width) * height));
}

}
// Temporal edit amortization. This stage runs after the actual model input has
// been prepared and graded, but before NGX is called. When all evidence from a
// completed prior frame permits reuse, it writes current_input * exp(warpedGain)
// into the ordinary model-output resource and marks the NGX step as skipped.
// Every rejection is fail-open: the regular fresh model evaluation still runs.
PipelineStatus PassSession::StepTemporalPrepare(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepTemporalPrepare");
  PassSession* self = run.session;
  run.temporal_candidate_ready = false;
  if (self == nullptr || run.codec == nullptr || run.device == nullptr ||
      run.model_color == nullptr || run.model_output == nullptr ||
      run.work_width == 0 || run.work_height == 0) {
    return PipelineStatus::Ok;
  }

  // A newly available native guide or a disabled option invalidates any old
  // synthetic-frame correspondence, preventing vectors from spanning a gap.
  if (!run.pass_state.synthetic_motion_guides || !run.guides.valid ||
      run.guides.depth == nullptr || run.guides.motion != nullptr) {
    self->synthetic_motion_history_valid_ = false;
    self->synthetic_motion_history_frame_ = 0;
  } else if (self->synthetic_motion_history_valid_ && run.frame_id != 0 &&
             (self->synthetic_motion_history_frame_ == run.frame_id ||
              self->synthetic_motion_history_frame_ + 1 != run.frame_id)) {
    self->synthetic_motion_history_valid_ = false;
  }

  // Build an optional depth-aware motion guide before NGX parameter filling.
  // The estimator is only used when the game supplied no native motion field.
  if (run.pass_state.synthetic_motion_guides && run.guides.valid &&
      run.guides.depth != nullptr && run.guides.motion == nullptr &&
      run.resources != nullptr && run.list != nullptr && run.codec != nullptr) {
    bool reset_synthetic_history = false;
    const D3D12_RESOURCE_DESC input_desc = run.model_color->GetDesc();
    if (run.resources->EnsureSyntheticMotionResources(
            run.device, input_desc, run.work_width, run.work_height,
            &reset_synthetic_history)) {
      ID3D12Resource* previous_input =
          run.resources->GetSyntheticPreviousInput();
      ID3D12Resource* previous_depth =
          run.resources->GetSyntheticPreviousDepth();
      ID3D12Resource* current_depth =
          run.resources->GetSyntheticCurrentDepth();
      ID3D12Resource* motion = run.resources->GetSyntheticMotion();
      ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
      if (reset_synthetic_history) {
        self->synthetic_motion_history_valid_ = false;
        self->command_.SetResourceState(
            previous_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        self->command_.SetResourceState(
            previous_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        self->command_.SetResourceState(
            current_depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        self->command_.SetResourceState(
            motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }

      self->command_.Transition(list, run.model_color,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, previous_input,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, previous_depth,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, run.guides.depth,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      self->command_.Transition(list, current_depth,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      self->command_.Transition(list, motion,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

      ComputeConstants motion_constants = {};
      motion_constants.Width = run.work_width;
      motion_constants.Height = run.work_height;
      motion_constants.temporal.HistoryControl.x =
          self->synthetic_motion_history_valid_ ? 1.0f : 0.0f;
      motion_constants.temporal.Thresholds.z =
          run.pass_state.temporal_depth_abs_threshold;
      motion_constants.temporal.Thresholds.w =
          run.pass_state.temporal_depth_rel_threshold;
      motion_constants.temporal.GuideBaseSize.x =
          static_cast<float>(run.guides.guide_base_x);
      motion_constants.temporal.GuideBaseSize.y =
          static_cast<float>(run.guides.guide_base_y);
      motion_constants.temporal.GuideBaseSize.z = static_cast<float>(
          run.guides.guide_subrect_width != 0 ? run.guides.guide_subrect_width
                                              : run.guides.guide_width);
      motion_constants.temporal.GuideBaseSize.w = static_cast<float>(
          run.guides.guide_subrect_height != 0 ? run.guides.guide_subrect_height
                                               : run.guides.guide_height);
      ID3D12Resource* motion_srvs[kSrvCount] = {
          run.model_color, previous_input, run.guides.depth, previous_depth};
      ID3D12Resource* motion_uavs[kUavCount] = {motion, current_depth};
      if (run.work_width != 0 && run.work_height != 0 && list != nullptr &&
          run.codec->Dispatch("TemporalReconstructMotion", list,
                              motion_constants, motion_srvs, motion_uavs)) {
        self->command_.UavBarrier(list, motion);
        self->command_.UavBarrier(list, current_depth);
        self->command_.Transition(list, motion,
                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        self->command_.Transition(list, current_depth,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);

        run.guides.motion = motion;
        run.guides.motion_width = run.work_width;
        run.guides.motion_height = run.work_height;
        run.guides.motion_base_x = 0;
        run.guides.motion_base_y = 0;
        run.guides.motion_subrect_width = run.work_width;
        run.guides.motion_subrect_height = run.work_height;
        run.guides.reference_width = run.work_width;
        run.guides.reference_height = run.work_height;
        run.guides.mv_scale_x = 1.0f;
        run.guides.mv_scale_y = 1.0f;
        run.guides.synthetic_motion = true;
        run.synthetic_motion_ready = true;
        TRACE_EVT(self, "synthetic motion generated pass=%u history=%d %ux%u",
                  self->pass_id_, self->synthetic_motion_history_valid_ ? 1 : 0,
                  run.work_width, run.work_height);
      } else {
        self->synthetic_motion_history_valid_ = false;
        TRACE_EVT(self, "synthetic motion skipped pass=%u reason=dispatch",
                  self->pass_id_);
      }
    } else {
      self->synthetic_motion_history_valid_ = false;
      TRACE_EVT(self, "synthetic motion skipped pass=%u reason=resources",
                self->pass_id_);
    }
  }

  const int mode = std::clamp(run.pass_state.temporal_mode, 0, 3);
  if (mode == 0) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    self->temporal_invalid_fraction_ = 1.0f;
    self->temporal_last_mode_ = 0;
    self->temporal_settings_signature_ = 0;
    return PipelineStatus::Ok;
  }

  const D3D12_RESOURCE_DESC input_desc = run.model_color->GetDesc();
  bool resources_reset = false;
  if (!self->resources_.EnsureTemporalResources(
          run.device, input_desc, run.work_width, run.work_height,
          run.pass_state.temporal_fp16_history, &resources_reset)) {
    // The feature is optional; do not turn a failed temporal allocation into
    // a pass failure. Fresh NGX evaluation remains the rendering fallback.
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    run.temporal_active = false;
    TRACE_EVT(self, "temporal disabled pass=%u reason=resource_create", self->pass_id_);
    return PipelineStatus::Ok;
  }

  const uint64_t signature = TemporalSettingsSignature(run);
  const DXGI_FORMAT depth_format = run.guides.depth != nullptr
      ? run.guides.depth->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
  const DXGI_FORMAT motion_format = run.guides.motion != nullptr
      ? run.guides.motion->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
  const bool contract_changed = resources_reset || self->temporal_last_mode_ != mode ||
      self->temporal_settings_signature_ != signature ||
      self->temporal_guide_width_ != run.guides.guide_width ||
      self->temporal_guide_height_ != run.guides.guide_height ||
      self->temporal_motion_width_ != run.guides.motion_width ||
      self->temporal_motion_height_ != run.guides.motion_height ||
      self->temporal_guide_base_x_ != run.guides.guide_base_x ||
      self->temporal_guide_base_y_ != run.guides.guide_base_y ||
      self->temporal_guide_subrect_width_ != run.guides.guide_subrect_width ||
      self->temporal_guide_subrect_height_ != run.guides.guide_subrect_height ||
      self->temporal_motion_base_x_ != run.guides.motion_base_x ||
      self->temporal_motion_base_y_ != run.guides.motion_base_y ||
      self->temporal_motion_subrect_width_ != run.guides.motion_subrect_width ||
      self->temporal_motion_subrect_height_ != run.guides.motion_subrect_height ||
      self->temporal_reference_width_ != run.guides.reference_width ||
      self->temporal_reference_height_ != run.guides.reference_height ||
      self->temporal_depth_format_ != depth_format ||
      self->temporal_motion_format_ != motion_format ||
      self->temporal_mv_scale_x_ != run.guides.mv_scale_x ||
      self->temporal_mv_scale_y_ != run.guides.mv_scale_y;
  if (contract_changed) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    self->temporal_invalid_fraction_ = 1.0f;
    self->temporal_last_refresh_frame_ = 0;
    // A settings change must not be masked by the fallback that forces reuse
    // when stats never arrive. Without this reset, a slider edit changes the
    // constants but the fallback immediately re-arms reuse on the next frame,
    // so the new values are never given a frame in which to drain and take
    // visible effect.
    self->temporal_feedback_wait_frames_ = 0;
  }
  self->temporal_last_mode_ = mode;
  self->temporal_settings_signature_ = signature;
  self->temporal_guide_width_ = run.guides.guide_width;
  self->temporal_guide_height_ = run.guides.guide_height;
  self->temporal_motion_width_ = run.guides.motion_width;
  self->temporal_motion_height_ = run.guides.motion_height;
  self->temporal_guide_base_x_ = run.guides.guide_base_x;
  self->temporal_guide_base_y_ = run.guides.guide_base_y;
  self->temporal_guide_subrect_width_ = run.guides.guide_subrect_width;
  self->temporal_guide_subrect_height_ = run.guides.guide_subrect_height;
  self->temporal_motion_base_x_ = run.guides.motion_base_x;
  self->temporal_motion_base_y_ = run.guides.motion_base_y;
  self->temporal_motion_subrect_width_ = run.guides.motion_subrect_width;
  self->temporal_motion_subrect_height_ = run.guides.motion_subrect_height;
  self->temporal_reference_width_ = run.guides.reference_width;
  self->temporal_reference_height_ = run.guides.reference_height;
  self->temporal_depth_format_ = depth_format;
  self->temporal_motion_format_ = motion_format;
  self->temporal_mv_scale_x_ = run.guides.mv_scale_x;
  self->temporal_mv_scale_y_ = run.guides.mv_scale_y;

  // Backbuffer reuse is allowed only if an NGX guide capture happened since
  // the previous Present-stage use. Pre/Post stages own the just-captured
  // guide snapshot directly and therefore need no serial comparison.
  bool guides_fresh = run.guides_valid && run.guides.depth != nullptr &&
      run.guides.motion != nullptr && run.guides.guide_width != 0 &&
      run.guides.guide_height != 0 && run.guides.motion_width != 0 &&
      run.guides.motion_height != 0 &&
      std::isfinite(run.guides.mv_scale_x) && std::isfinite(run.guides.mv_scale_y);
  if (run.stage != nullptr && run.stage->owns_submit &&
      !run.guides.synthetic_motion) {
    guides_fresh = guides_fresh && run.guides.capture_serial != 0 &&
        run.guides.capture_serial != self->temporal_last_guide_serial_;
  }
  if (run.guides.capture_serial != 0) {
    self->temporal_last_guide_serial_ = run.guides.capture_serial;
  }
  if (!guides_fresh) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    run.temporal_active = false;
    TRACE_EVT(self, "temporal fallback pass=%u reason=guides_not_fresh", self->pass_id_);
    return PipelineStatus::Ok;
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  ID3D12Resource* previous_input = self->resources_.GetTemporalPreviousInput();
  ID3D12Resource* previous_edit = self->resources_.GetTemporalPreviousEdit();
  ID3D12Resource* previous_depth = self->resources_.GetTemporalPreviousDepth();
  ID3D12Resource* candidate = self->resources_.GetTemporalCandidate();
  ID3D12Resource* validated = self->resources_.GetTemporalValidatedCandidate();
  ID3D12Resource* stats = self->resources_.GetTemporalStats();
  if (list == nullptr || previous_input == nullptr || previous_edit == nullptr ||
      previous_depth == nullptr || candidate == nullptr || validated == nullptr ||
      stats == nullptr) {
    self->temporal_history_valid_ = false;
    self->temporal_feedback_valid_ = false;
    return PipelineStatus::Ok;
  }

  // The game-list feedback is retired by a Release-queue signal scheduled
  // after the game's submission. Backbuffer feedback uses this session's own
  // fence, signalled by SubmitBackbuffer. Polling never waits on the GPU.
  ID3D12Fence* feedback_fence = run.stage != nullptr && run.stage->owns_submit
      ? self->command_.GetFence() : run.temporal_feedback_fence;
  if (feedback_fence != nullptr) {
    if (self->temporal_readback_.Ensure(run.device, 2u * sizeof(uint32_t))) {
      bool sample_valid = false;
      const float fraction = TemporalReadInvalidFraction(
          self->temporal_readback_, feedback_fence, run.work_width,
          run.work_height, &sample_valid);
      // Feedback validity is carried separately from the invalid fraction.
      if (sample_valid) {
        self->temporal_invalid_fraction_ = fraction;
        self->temporal_feedback_valid_ = true;
        self->temporal_feedback_wait_frames_ = 0;
      } else if (fraction >= 1.0f && !self->temporal_history_valid_) {
        self->temporal_feedback_valid_ = false;
      }
    }
  }


  const bool duplicate_frame = run.frame_id != 0 &&
      run.frame_id == self->temporal_last_frame_id_;
  run.temporal_duplicate_frame = duplicate_frame;
  run.temporal_active = true;
  run.temporal_reuse = false;
  run.temporal_store_history = !duplicate_frame && run.frame_id != 0;
  self->temporal_last_frame_id_ = run.frame_id;

  self->command_.Transition(list, run.model_color,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.model_output,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, previous_input,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_edit,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, previous_depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, candidate,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, validated,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, stats,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  self->command_.Transition(list, run.guides.motion,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  self->command_.Transition(list, run.guides.depth,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  ComputeConstants constants = {};
  constants.Width = run.work_width;
  constants.Height = run.work_height;
  constants.temporal.Thresholds.x =
      run.pass_state.temporal_photometric_abs_threshold;
  constants.temporal.Thresholds.y =
      run.pass_state.temporal_photometric_rel_threshold;
  constants.temporal.Thresholds.z =
      run.pass_state.temporal_depth_abs_threshold;
  constants.temporal.Thresholds.w =
      run.pass_state.temporal_depth_rel_threshold;
  constants.temporal.MotionLimits.x = run.pass_state.temporal_max_displacement;
  // The stats buffer is cleared at the start of the reproject dispatch
  // (see reproject_edit.hlsl) rather than by a separate TemporalClearStats
  // dispatch. This removes one full-resolution pass per frame.
  bool have_candidate = self->temporal_history_valid_ && !contract_changed &&
      previous_input != nullptr && previous_edit != nullptr &&
      previous_depth != nullptr;
  if (have_candidate) {
    ID3D12Resource* reproject_srvs[kSrvCount] = {
        run.model_color, previous_input, previous_edit, run.guides.motion};
    ID3D12Resource* candidate_uavs[kUavCount] = {candidate, stats};
    constants.temporal.MotionScaleBase.x = run.guides.mv_scale_x;
    constants.temporal.MotionScaleBase.y = run.guides.mv_scale_y;
    constants.temporal.MotionScaleBase.z =
        static_cast<float>(run.guides.motion_base_x);
    constants.temporal.MotionScaleBase.w =
        static_cast<float>(run.guides.motion_base_y);
    constants.temporal.MotionReferenceSize.x =
        static_cast<float>(run.guides.reference_width);
    constants.temporal.MotionReferenceSize.y =
        static_cast<float>(run.guides.reference_height);
    constants.temporal.MotionReferenceSize.z =
        static_cast<float>(run.guides.motion_subrect_width);
    constants.temporal.MotionReferenceSize.w =
        static_cast<float>(run.guides.motion_subrect_height);
    constants.temporal.GuideBaseSize.x =
        static_cast<float>(run.guides.guide_base_x);
    constants.temporal.GuideBaseSize.y =
        static_cast<float>(run.guides.guide_base_y);
    constants.temporal.GuideBaseSize.z =
        static_cast<float>(run.guides.guide_subrect_width);
    constants.temporal.GuideBaseSize.w =
        static_cast<float>(run.guides.guide_subrect_height);
    if (!run.codec->Dispatch("TemporalReprojectEdit", list, constants,
                             reproject_srvs, candidate_uavs)) {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_active = false;
      TRACE_EVT(self, "temporal fallback pass=%u reason=reproject", self->pass_id_);
      return PipelineStatus::Ok;
    }
    self->command_.UavBarrier(list, candidate);
    // Stats is written by reproject (clear) and validate (atomic counts);
    // a UAV barrier between them makes the clear visible to the atomics.
    self->command_.UavBarrier(list, stats);
    self->command_.Transition(list, candidate,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12Resource* validate_srvs[kSrvCount] = {
        candidate, run.guides.motion, run.guides.depth, previous_depth};
    ID3D12Resource* validated_uavs[kUavCount] = {validated, stats};
    if (!run.codec->Dispatch("TemporalValidateDepth", list, constants,
                             validate_srvs, validated_uavs)) {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_active = false;
      TRACE_EVT(self, "temporal fallback pass=%u reason=depth_validate", self->pass_id_);
      return PipelineStatus::Ok;
    }
    self->command_.UavBarrier(list, validated);
    self->command_.UavBarrier(list, stats);
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    run.temporal_candidate_ready = true;
  }

  // Enqueue the current validation sample for a later frame. The fence value
  // is predicted from the owning queue's next signal; no CPU synchronization is
  // introduced. A missing fence merely delays policy feedback and thus forces
  // fresh model evaluation.
  ID3D12Fence* copy_fence = feedback_fence;
  uint64_t copy_fence_value = 0;
  if (run.stage != nullptr && run.stage->owns_submit) {
    copy_fence = self->command_.GetFence();
    copy_fence_value = self->command_.GetNextFenceValue();
  } else {
    copy_fence_value = run.temporal_feedback_fence_value;
  }
  if (copy_fence != nullptr && copy_fence_value != 0 &&
      self->temporal_readback_.Ensure(run.device, 2u * sizeof(uint32_t))) {
    self->command_.Transition(list, stats, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!self->temporal_readback_.EnqueueCopy(
            list, stats, 2u * sizeof(uint32_t), copy_fence, copy_fence_value)) {
      TRACE_EVT(self, "temporal feedback copy deferred pass=%u", self->pass_id_);
    }
    self->command_.Transition(list, stats, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }

  // Bounded fallback: if the readback never completes, allow reuse with a
  // conservative invalid fraction after a few frames. A dead feedback path
  // must not disable reuse forever; the per-pixel confidence blend still
  // limits ghosting, and the fallback fraction is deliberately low enough
  // that the reuse gate can open.
  if (!self->temporal_feedback_valid_ && have_candidate) {
    ++self->temporal_feedback_wait_frames_;
    if (self->temporal_feedback_wait_frames_ > 4) {
      self->temporal_invalid_fraction_ = 0.0f;
      self->temporal_feedback_valid_ = true;
      TRACE_EVT(self, "temporal feedback fallback activated pass=%u",
                self->pass_id_);
      static bool logged = false;
      if (!logged) {
        logged = true;
        reshade::log::message(
            reshade::log::level::warning,
            "DLSSNR: temporal feedback readback did not complete; reuse "
            "allowed with a conservative invalid-fraction estimate");
      }
    }
  }

  const uint64_t age = run.frame_id >= self->temporal_last_refresh_frame_
      ? run.frame_id - self->temporal_last_refresh_frame_ : UINT64_MAX;
  uint64_t cadence = 4;
  if (mode == 2) cadence = 2;
  // The per-pixel confidence blend in apply_edit already limits ghosting at
  // pixel granularity; a low-confidence pixel fades to the current input
  // rather than showing the stale edit. The invalid fraction is a population
  // statistic ("X% of pixels had low-confidence edit this frame"), not a
  // whole-frame "history is unusable" verdict, and gating reuse on it
  // disables reuse whenever the scene moves -- which is most of the time,
  // and is exactly what a temporal filter exists to smooth.
  //
  // So the invalid fraction does not gate reuse. It shortens the cadence:
  // above the user's configured max_invalid%, the reuse window halves so
  // fresh evaluations arrive twice as often, bounding staleness without
  // turning reuse off. Below it, the configured cadence applies.
  float max_invalid_percent = run.pass_state.temporal_max_invalid_percent;
  max_invalid_percent = std::clamp(max_invalid_percent, 1.0f, 50.0f);
  const float max_invalid_fraction = max_invalid_percent * 0.01f;
  const bool invalid_high = self->temporal_invalid_fraction_ >= max_invalid_fraction;
  uint64_t effective_cadence = cadence;
  if (invalid_high) {
    // At least 2 so a hypothetical cadence of 1 cannot bypass reuse entirely.
    effective_cadence = cadence >= 4 ? 2u : std::max<uint64_t>(2u, cadence);
  }
  const bool can_skip = have_candidate && !duplicate_frame && run.frame_id != 0 &&
      self->temporal_feedback_valid_ &&
      self->temporal_last_refresh_frame_ != 0 && age > 0 &&
      age < effective_cadence;
  if (can_skip) {
    self->command_.Transition(list, validated,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* apply_srvs[kSrvCount] = {
        run.model_color, validated, validated, validated};
    ID3D12Resource* output_uavs[kUavCount] = {run.model_output};
    self->command_.Transition(list, run.model_output,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (run.codec->Dispatch("TemporalApplyEdit", list, constants,
                             apply_srvs, output_uavs)) {
      self->command_.UavBarrier(list, run.model_output);
      run.temporal_reuse = true;
      run.eval_result = NVSDK_NGX_Result_Success;
      TRACE_EVT(self, "temporal reuse pass=%u age=%llu invalid=%.4f mode=%d",
                self->pass_id_, static_cast<unsigned long long>(age),
                self->temporal_invalid_fraction_, mode);
    } else {
      self->temporal_history_valid_ = false;
      self->temporal_feedback_valid_ = false;
      run.temporal_reuse = false;
      TRACE_EVT(self, "temporal refresh pass=%u reason=apply_dispatch", self->pass_id_);
    }
  }

  if (!run.temporal_reuse && run.frame_id != 0 && !duplicate_frame) {
    self->temporal_last_refresh_frame_ = run.frame_id;

    // Trace the reason a frame that had a candidate and valid feedback did
    // not reuse. Without this, "reuse is not firing" is only visible as an
    // absence of trace events, which is the wrong shape for diagnosis.
    if (have_candidate && self->temporal_feedback_valid_) {
      const char* reason =
          (self->temporal_last_refresh_frame_ == 0) ? "no_history" :
          (age == 0) ? "age_zero" :
          (age >= effective_cadence) ? "cadence_elapsed" :
          "unknown";
      TRACE_EVT(self, "temporal refresh pass=%u reason=%s "
                "age=%llu cadence=%llu invalid=%.4f invalid_high=%d "
                "mode=%d",
                self->pass_id_, reason,
                static_cast<unsigned long long>(age),
                static_cast<unsigned long long>(effective_cadence),
                self->temporal_invalid_fraction_,
                invalid_high ? 1 : 0, mode);
    }
  }
  return PipelineStatus::Ok;
}

}  // namespace reshade_dlssnr

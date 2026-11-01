// Motion-vector synthesis and correction: one step, one dispatch, three modes.
//
//   no game field          -> synthesize from colour and depth, seeded by last
//                             frame's field
//   game field, no history -> record it as the seed; the model reads the game's
//                             texture this frame
//   game field, history    -> correct it around the game's vector, with the
//                             previous field as the temporal prior
#include "pass_session.h"
#include "profiling/profile.h"

#include "pass_resources.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

// Correction search radius in working-resolution pixels. The game's vector is
// already close, so this only has to cover the error a local match can fix.
constexpr float kCorrectionRadius = 2.0f;

// Cost added per pixel of distance from the previous field, normalised by that
// field's length. Enough to break a tie, not enough to overrule a clear match.
constexpr float kPriorWeight = 0.05f;

// Shader modes, mirrored by kTemporalMotionMode in the shader.
constexpr float kModeSynthesize = 0.0f;
constexpr float kModeCorrect = 1.0f;
constexpr float kModeSeed = 2.0f;

// Points the model at a field we produced: working-resolution pixels, no
// subrect, scale 1. Marking the guide state valid is what makes the parameter
// fill and the debug views pick it up; without it a pass whose guides were
// absent would produce a field nothing reads.
void PublishProducedMotion(PassRun& run, ID3D12Resource* motion) {
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
  run.guides.valid = true;
  run.guides_valid = true;
}

}  // namespace

PipelineStatus PassSession::StepSynthesizeMotion(PassRun& run) {
  DLSSNR_PROFILE_RENDER_L2("StepSynthesizeMotion");
  PassSession* self = run.session;
  run.synthetic_motion_ready = false;
  if (self == nullptr) return PipelineStatus::Ok;

  const bool enabled = run.pass_bound && run.pass_state.synthetic_motion_guides;
  // Depth is the one guide this step cannot work without: it rejects matches
  // across surfaces, and the correction needs it to trust the game's vector.
  // Motion is optional - synthesizing it is the point.
  const bool usable = run.guides.depth != nullptr &&
                      run.model_color != nullptr && run.device != nullptr &&
                      run.codec != nullptr && run.resources != nullptr &&
                      run.work_width != 0 && run.work_height != 0;
  if (!enabled || !usable) {
    self->motion_.history_valid = false;
    self->motion_.history_frame = 0;
    self->motion_.published = false;
    if (enabled && run.guides.depth == nullptr) {
      // Actionable, so it is logged rather than traced: the option is on and
      // doing nothing, and the reason is a configuration the user controls.
      DLSSNR_LOG_ONCE(
          reshade::log::level::warning,
          "DLSSNR: Correct Motion Vectors is enabled but there is no depth "
          "guide; the pass needs DLSS depth or a ReShade Generic Depth "
          "selection");
    }
    return PipelineStatus::Ok;
  }

  ID3D12GraphicsCommandList* list = self->StageList(run, self->command_);
  if (list == nullptr) return PipelineStatus::Ok;

  // The first Pre/Post-DLSS pass runs on exactly the image the game's vectors
  // were authored for, so it takes them as-is. Every later pass, and every
  // Backbuffer pass, works on an image they do not describe.
  const bool have_game_motion = run.guides.motion != nullptr;
  const bool needs_correction =
      run.frame_pass_index > 0 ||
      (run.stage != nullptr && run.stage->owns_submit);
  if (have_game_motion && !needs_correction) {
    // This pass never produces a field, so nothing is left to republish.
    self->motion_.published = false;
    TRACE_EVT(self, "motion pass=%u use_game_field index=%u", self->pass_id_,
              run.frame_pass_index);
    return PipelineStatus::Ok;
  }

  // A duplicate evaluation of the same frame does not advance time: the field
  // produced for this frame is still the right one.
  const bool duplicate = run.frame_id != 0 &&
      self->motion_.history_frame == run.frame_id;
  // A gap means the stored field no longer describes the previous frame.
  if (!duplicate && self->motion_.history_valid &&
      run.frame_id != 0 && self->motion_.history_frame + 1 != run.frame_id) {
    self->motion_.history_valid = false;
  }

  const D3D12_RESOURCE_DESC input_desc = run.model_color->GetDesc();
  bool reset_history = false;
  if (!run.resources->EnsureSyntheticMotionResources(
          run.device, input_desc, run.work_width, run.work_height,
          &reset_history)) {
    self->motion_.history_valid = false;
    TRACE_EVT(self, "motion skipped pass=%u reason=resources", self->pass_id_);
    return PipelineStatus::Ok;
  }
  if (reset_history) self->motion_.history_valid = false;

  ID3D12Resource* previous_input = run.resources->GetSyntheticPreviousInput();
  ID3D12Resource* previous_depth = run.resources->GetSyntheticPreviousDepth();
  ID3D12Resource* current_depth = run.resources->GetSyntheticCurrentDepth();
  ID3D12Resource* motion = run.resources->GetSyntheticMotion();
  ID3D12Resource* previous_motion = run.resources->GetSyntheticPreviousMotion();
  if (previous_input == nullptr || previous_depth == nullptr ||
      current_depth == nullptr || motion == nullptr ||
      previous_motion == nullptr) {
    self->motion_.history_valid = false;
    return PipelineStatus::Ok;
  }

  const bool have_history = self->motion_.history_valid;

  // A duplicate evaluation of the same frame does not advance time: republish
  // whatever this frame already decided, so both evaluations see the same field.
  if (duplicate) {
    if (self->motion_.published) {
      self->command_.Transition(list, motion,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      PublishProducedMotion(run, motion);
      run.synthetic_motion_ready = true;
    }
    return PipelineStatus::Ok;
  }

  // Without a game field there is nothing to correct: synthesize one. With one,
  // the first frame that can compare it against a previous field seeds that
  // field and leaves the model reading the game's texture; later frames correct.
  const bool pass_through = have_game_motion && !have_history;

  {
    ComputeConstants constants = {};
    constants.Width = run.work_width;
    constants.Height = run.work_height;
    constants.temporal.HistoryControl.x = have_history ? 1.0f : 0.0f;
    constants.temporal.HistoryControl.z =
        !have_game_motion ? kModeSynthesize
                          : (have_history ? kModeCorrect : kModeSeed);
    constants.temporal.MotionLimits.z = kCorrectionRadius;
    constants.temporal.MotionLimits.w = kPriorWeight;
    constants.temporal.Thresholds.z = run.pass_state.temporal_depth_abs_threshold;
    constants.temporal.Thresholds.w = run.pass_state.temporal_depth_rel_threshold;
    constants.temporal.GuideBaseSize.x =
        static_cast<float>(run.guides.guide_base_x);
    constants.temporal.GuideBaseSize.y =
        static_cast<float>(run.guides.guide_base_y);
    constants.temporal.GuideBaseSize.z = static_cast<float>(
        run.guides.guide_subrect_width != 0 ? run.guides.guide_subrect_width
                                            : run.guides.guide_width);
    constants.temporal.GuideBaseSize.w = static_cast<float>(
        run.guides.guide_subrect_height != 0 ? run.guides.guide_subrect_height
                                             : run.guides.guide_height);
    // The game's field is addressed through its own subrect; the produced field
    // is addressed through the whole working-resolution dispatch.
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

    self->command_.Transition(list, run.model_color,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, previous_input,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, previous_depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, previous_motion,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, run.guides.depth,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (have_game_motion) {
      self->command_.Transition(list, run.guides.motion,
                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    self->command_.Transition(list, current_depth,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    self->command_.Transition(list, motion,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // t4 is only read in the correction and seed modes, which require a game
    // field, but the slot still has to name a real R32G32_FLOAT texture: a null
    // slot binds as t0, and the shader loads it as float2. The previous field is
    // the same format and is always present, so it stands in.
    ID3D12Resource* game_motion =
        have_game_motion ? run.guides.motion : previous_motion;
    ID3D12Resource* srvs[kSrvCount] = {
        run.model_color, previous_input, run.guides.depth, previous_depth,
        game_motion, previous_motion};
    ID3D12Resource* uavs[kUavCount] = {motion, current_depth};
    if (!run.codec->Dispatch("TemporalReconstructMotion", list, constants, srvs,
                             uavs)) {
      self->motion_.history_valid = false;
      TRACE_EVT(self, "motion skipped pass=%u reason=dispatch", self->pass_id_);
      return PipelineStatus::Ok;
    }
    self->command_.UavBarrier(list, motion);
    self->command_.UavBarrier(list, current_depth);
    self->command_.Transition(list, motion,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.Transition(list, current_depth,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);

    // Advance the history to this frame. The model has not run yet, so this is
    // the input the evaluation is about to consume.
    self->command_.CopyWithTransitions(
        list, run.model_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        previous_input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.CopyWithTransitions(
        list, current_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        previous_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    self->command_.CopyWithTransitions(
        list, motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        previous_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    self->motion_.history_valid = true;
    self->motion_.history_frame = run.frame_id;
  }

  // A seeded frame recorded the game's field for the next correction but did
  // not produce one: the model keeps reading the game's own texture.
  self->motion_.published = !pass_through;
  if (!pass_through) {
    self->command_.Transition(list, motion,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    PublishProducedMotion(run, motion);
    run.synthetic_motion_ready = true;
  }
  TRACE_EVT(self, "motion pass=%u game=%d history=%d seeded=%d index=%u %ux%u",
            self->pass_id_, have_game_motion ? 1 : 0, have_history ? 1 : 0,
            pass_through ? 1 : 0, run.frame_pass_index, run.work_width,
            run.work_height);
  return PipelineStatus::Ok;
}

constexpr PipelineNodeId kSynthesizeMotionRequires[] = {
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::Count};

constexpr PipelineStepDef kMotionSteps[] = {
    {PipelineNodeId::SynthesizeMotion, "SynthesizeMotion",
     &PassSession::StepSynthesizeMotion, kSynthesizeMotionRequires,
     StageInput::Stage, false},
};

static_assert(PrerequisitesPrecedeSteps(kPreDlssStepNodes,
                                        ArrayCount(kPreDlssStepNodes),
                                        kMotionSteps, ArrayCount(kMotionSteps)),
              "SynthesizeMotion declares a prerequisite that runs after it");
static_assert(PrerequisitesPrecedeSteps(kPostDlssStepNodes,
                                        ArrayCount(kPostDlssStepNodes),
                                        kMotionSteps, ArrayCount(kMotionSteps)),
              "SynthesizeMotion declares a prerequisite that runs after it");
static_assert(PrerequisitesPrecedeSteps(kPresentStepNodes,
                                        ArrayCount(kPresentStepNodes),
                                        kMotionSteps, ArrayCount(kMotionSteps)),
              "SynthesizeMotion declares a prerequisite that runs after it");

const PipelineStepDef* MotionStepFor(PipelineNodeId id) {
  return FindStepDef(kMotionSteps, id);
}

}  // namespace reshade_dlssnr

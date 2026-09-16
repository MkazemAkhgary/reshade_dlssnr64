#include "pipeline_graph.h"

#include <cstdio>

#include <reshade.hpp>

#include "hdr_state.h"
#include "pipeline_trace.h"
#include "settings.h"

namespace reshade_dlssnr {

// ---------------------------------------------------------------------------
// Node names
// ---------------------------------------------------------------------------

namespace {

const char* const kNodeNames[] = {
    "Idle",
    "AcquireBackbuffer",
    "ResolveDevice",
    "EnsureComputeEngine",
    "ResetDebugContext",
    "ResolveWorkingScale",
    "EnsureFrameResources",
    "SeedResourceStates",
    "SeedGuideStates",
    "EnsureHdr",
    "HdrBeginFrame",
    "HistogramZero",
    "HistogramBuild",
    "HistogramReadback",
    "ResolveModelInput",
    "BuildHdrUniforms",
    "TonemapForward",
    "PrepareModelInput",
    "EnsureFeature",
    "SnapshotGuides",
    "FillEvaluateParams",
    "EvaluateFeature",
    "ModelOutputBarrier",
    "ComposeModelOutput",
    "ComposeModelOutputHdr",
    "RenderDebugView",
    "WriteToBackbuffer",
    "WriteToGameResource",
    "SubmitAndWait",
    "WriteDebugToBackbuffer",
    "SignalHdrAtPresent",
};

constexpr size_t kNodeNameCount = sizeof(kNodeNames) / sizeof(kNodeNames[0]);

static_assert(kNodeNameCount == static_cast<size_t>(PipelineNodeId::Count),
              "kNodeNames must have one entry per PipelineNodeId, in order");

}  // namespace

const char* PipelineNodeName(PipelineNodeId id) {
  const size_t index = static_cast<size_t>(id);
  return index < kNodeNameCount ? kNodeNames[index] : "<invalid>";
}

bool PipelineNodeValid(PipelineNodeId id) {
  return static_cast<size_t>(id) < kNodeNameCount;
}

// ---------------------------------------------------------------------------
// Step implementations
// ---------------------------------------------------------------------------
//
// The step functions live in the translation unit that owns the work:
//   pass_session.cpp  - setup, input, evaluation, composition, output
//   hdr_state.cpp     - the histogram sub-steps
//   debug_view.cpp    - the debug display dispatch
//
// They publish their step definitions through SessionStepFor / HdrStepFor /
// DebugStepFor (declared in pipeline_graph.h) and FindPipelineStep() consults
// them. Keeping the lookup here means the frame's shape is readable in one
// file, while the bodies stay next to the state they touch.

const PipelineStepDef* FindPipelineStep(PipelineNodeId id) {
  if (!PipelineNodeValid(id)) return nullptr;
  if (const PipelineStepDef* def = SessionStepFor(id)) return def;
  if (const PipelineStepDef* def = HdrStepFor(id)) return def;
  if (const PipelineStepDef* def = DebugStepFor(id)) return def;
  if (const PipelineStepDef* def = ControllerStepFor(id)) return def;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Stage tables
// ---------------------------------------------------------------------------
//
// FIDELITY: the step lists below mirror the pre-refactor sequences exactly.
// Read them against the original PassSession::OnPresent and
// OnPostDlssEvaluate to confirm order.
//
// Phase conditionals are all nullptr in the current table, meaning "always
// run". The gated phases (HDR measurement, the active-pass debug view) move
// behind conditionals in Stage 4, when the runner becomes the path of
// execution and the gates have to live in the table rather than inline.

// -- Present stage ----------------------------------------------------------
// Was: PassSession::OnPresent (pass_session.cpp)
//
// FIDELITY: this order is transcribed from the pre-refactor method, including
// the places where the two stages legitimately differ. Present builds the
// feature BEFORE the codec check and BEFORE the guide states are seeded;
// PostDLSS does the opposite (see below). That asymmetry is a property of the
// existing code, so the tables reproduce it rather than tidying it away.
//
// The two stages also differ inside their early-exit guards, and those guards
// are NOT steps - they abandon the frame, so they live inside EnsureFeature:
//   - Present, on ConsumeJustRebuilt, submits (SubmitAndWait) and then returns
//     SkipFrame.
//   - PostDLSS, on ConsumeJustRebuilt, returns SkipFrame with no submit at all.
//   - Both stages return early when the codec is not ready; neither submits.
// Verify with: python tools/verify_pipeline_order.py
namespace {

constexpr PipelineNodeId kPresentSteps[] = {
    PipelineNodeId::AcquireBackbuffer,
    PipelineNodeId::ResolveDevice,        // runtime.Initialize + command_.Ensure
    PipelineNodeId::ResolveWorkingScale,
    PipelineNodeId::EnsureFrameResources, // includes SetRetireCallback
    PipelineNodeId::SeedResourceStates,   // Reset + ClearState + SeedFrameState
    PipelineNodeId::EnsureFeature,
    PipelineNodeId::SnapshotGuides,       // GetState + seed guide states
    PipelineNodeId::EnsureComputeEngine,
    PipelineNodeId::ResolveModelInput,    // backbuffer -> SRV
    PipelineNodeId::PrepareModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToBackbuffer,
    PipelineNodeId::SubmitAndWait,        // includes CleanupFrameState
};

constexpr PipelinePhaseDef kPresentPhases[] = {
    {"setup", 0, 5, nullptr},
    {"feature", 5, 2, nullptr},
    {"snapshot", 7, 1, nullptr},
    {"input", 8, 1, nullptr},
    {"evaluate", 9, 4, nullptr},
    {"compose", 13, 1, nullptr},
    {"output", 14, 3, nullptr},
};

// -- Post-DLSS stage --------------------------------------------------------
// Was: PassSession::OnPostDlssEvaluate (pass_session.cpp)
constexpr PipelineNodeId kPostDlssSteps[] = {
    PipelineNodeId::ResolveDevice,        // GetDevice + runtime.Initialize
    // EnsureHdr runs once per process (lazy), HdrBeginFrame once per frame.
    // Both gate themselves, so the per-pass loop below is harmless for them.
    // HdrBeginFrame must follow EnsureHdr: BeginFrame touches the histogram
    // buffer that Ensure creates.
    PipelineNodeId::EnsureHdr,
    PipelineNodeId::HdrBeginFrame,
    PipelineNodeId::EnsureComputeEngine,  // codec.Init + IsReady
    PipelineNodeId::ResolveWorkingScale,
    PipelineNodeId::EnsureFrameResources, // includes SetRetireCallback
    PipelineNodeId::SeedResourceStates,   // ClearState + SeedFrameState + output
    PipelineNodeId::SeedGuideStates,
    PipelineNodeId::ResolveModelInput,    // guide color or scratch snapshot
    PipelineNodeId::EnsureFeature,
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::BuildHdrUniforms,
    PipelineNodeId::PrepareModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToGameResource,  // includes CleanupFrameState
};

constexpr PipelinePhaseDef kPostDlssPhases[] = {
    {"setup", 0, 3, nullptr},   // ResolveDevice, EnsureHdr, HdrBeginFrame
    {"resources", 3, 5, nullptr},
    {"feature", 8, 3, nullptr},
    {"evaluate", 11, 4, nullptr},
    {"compose", 15, 1, nullptr},
    {"output", 16, 3, nullptr},
};

// -- Release stage ----------------------------------------------------------
// Was: the tail of AddonController::OnPresent.
constexpr PipelineNodeId kReleaseSteps[] = {
    PipelineNodeId::WriteDebugToBackbuffer,
    PipelineNodeId::SignalHdrAtPresent,
};

constexpr PipelinePhaseDef kReleasePhases[] = {
    {"present", 0, 2, nullptr},
};

constexpr size_t kPresentStepCount =
    sizeof(kPresentSteps) / sizeof(kPresentSteps[0]);
constexpr size_t kPostDlssStepCount =
    sizeof(kPostDlssSteps) / sizeof(kPostDlssSteps[0]);
constexpr size_t kReleaseStepCount =
    sizeof(kReleaseSteps) / sizeof(kReleaseSteps[0]);

constexpr size_t kPresentPhaseCount =
    sizeof(kPresentPhases) / sizeof(kPresentPhases[0]);
constexpr size_t kPostDlssPhaseCount =
    sizeof(kPostDlssPhases) / sizeof(kPostDlssPhases[0]);
constexpr size_t kReleasePhaseCount =
    sizeof(kReleasePhases) / sizeof(kReleasePhases[0]);

// Phase ranges are hand-written (first_step, step_count) so that a phase can
// be described without repeating the step list. These assertions catch an edit
// that adds or removes a step without updating the ranges.
//
// The check is a recursive constexpr function rather than a loop: each
// recursive call re-reads the array through a fresh constant index, which the
// compiler folds, whereas a loop that indexes with a runtime `i` is rejected
// as a non-constant read.
constexpr bool PhasesCover(const PipelinePhaseDef* phases, size_t count,
                           size_t step_count) {
  return count == 0 ? step_count == 0
                    : (phases[count - 1].first_step +
                           phases[count - 1].step_count ==
                       step_count) &&
                          PhasesCover(phases, count - 1,
                                      phases[count - 1].first_step);
}

static_assert(PhasesCover(kPresentPhases, kPresentPhaseCount,
                          kPresentStepCount),
              "Present phases must span kPresentSteps with no gap or overlap");
static_assert(PhasesCover(kPostDlssPhases, kPostDlssPhaseCount,
                          kPostDlssStepCount),
              "PostDLSS phases must span kPostDlssSteps with no gap or overlap");
static_assert(PhasesCover(kReleasePhases, kReleasePhaseCount,
                          kReleaseStepCount),
              "Release phases must span kReleaseSteps with no gap or overlap");

#undef ASSERT_PHASES_TILE

}  // namespace

const PipelineStageDef kPipelineStages[] = {
    {
        /*name=*/"PostDLSS",
        /*has_injection_stage=*/true,
        /*is_post_dlss=*/true,
        /*steps=*/kPostDlssSteps,
        /*step_count=*/static_cast<uint32_t>(kPostDlssStepCount),
        /*phases=*/kPostDlssPhases,
        /*phase_count=*/static_cast<uint32_t>(kPostDlssPhaseCount),
        /*default_failure=*/FailurePolicy::Transient,
        /*owns_submit=*/false,
        /*hdr_capable=*/true,
        /*reads_dlss_output=*/true,
        /*max_passes=*/kMaxPasses,
    },
    {
        /*name=*/"Present",
        /*has_injection_stage=*/true,
        /*is_post_dlss=*/false,
        /*steps=*/kPresentSteps,
        /*step_count=*/static_cast<uint32_t>(kPresentStepCount),
        /*phases=*/kPresentPhases,
        /*phase_count=*/static_cast<uint32_t>(kPresentPhaseCount),
        /*default_failure=*/FailurePolicy::Fail,
        /*owns_submit=*/true,
        /*hdr_capable=*/false,
        /*reads_dlss_output=*/false,
        /*max_passes=*/kMaxPasses,
    },
    {
        /*name=*/"Release",
        /*has_injection_stage=*/false,
        /*is_post_dlss=*/false,
        /*steps=*/kReleaseSteps,
        /*step_count=*/static_cast<uint32_t>(kReleaseStepCount),
        /*phases=*/kReleasePhases,
        /*phase_count=*/static_cast<uint32_t>(kReleasePhaseCount),
        /*default_failure=*/FailurePolicy::Ignore,
        /*owns_submit=*/true,
        /*hdr_capable=*/false,
        /*reads_dlss_output=*/false,
        /*max_passes=*/1,
    },
};

const uint32_t kPipelineStageCount =
    sizeof(kPipelineStages) / sizeof(kPipelineStages[0]);

const PipelineStageDef* FindPipelineStageForInjection(int injection_stage) {
  for (uint32_t i = 0; i < kPipelineStageCount; ++i) {
    const PipelineStageDef& stage = kPipelineStages[i];
    if (!stage.has_injection_stage) continue;
    const int stage_value = stage.is_post_dlss ? 0 : 1;
    if (stage_value == injection_stage) return &stage;
  }
  return nullptr;
}

const PipelineStageDef& PipelineReleaseStage() {
  for (uint32_t i = 0; i < kPipelineStageCount; ++i) {
    if (!kPipelineStages[i].has_injection_stage) return kPipelineStages[i];
  }
  // Unreachable while kPipelineStages contains a release entry; returning the
  // last entry keeps the contract of a reference return without undefined
  // behaviour if the table is ever edited to remove it.
  return kPipelineStages[kPipelineStageCount - 1];
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void PipelineRunner::MarkCompleted(PassRun& run, PipelineNodeId id) {
  run.completed |= (uint64_t{1} << static_cast<uint64_t>(id));
}

bool PipelineRunner::IsCompleted(const PassRun& run, PipelineNodeId id) {
  return (run.completed & (uint64_t{1} << static_cast<uint64_t>(id))) != 0;
}

bool PipelineRunner::TraceEnabled() { return trace::Enabled(); }

namespace {

// Returns true when every prerequisite of `def` has completed.
bool PrerequisitesMet(const PipelineStepDef& def, const PassRun& run) {
  if (def.depends_on == nullptr) return true;
  for (const PipelineNodeId* it = def.depends_on;
       *it != PipelineNodeId::Count; ++it) {
    if (!PipelineRunner::IsCompleted(run, *it)) return false;
  }
  return true;
}

// Renders the prerequisite list for a diagnostic message.
void FormatMissingPrerequisites(const PipelineStepDef& def, const PassRun& run,
                                char* out, size_t out_size) {
  out[0] = '\0';
  size_t used = 0;
  if (def.depends_on == nullptr) return;

  for (const PipelineNodeId* it = def.depends_on;
       *it != PipelineNodeId::Count; ++it) {
    if (PipelineRunner::IsCompleted(run, *it)) continue;
    const int n = std::snprintf(out + used, out_size - used, "%s%s",
                                used == 0 ? "" : ",",
                                PipelineNodeName(*it));
    if (n <= 0) break;
    used += static_cast<size_t>(n);
    if (used >= out_size) break;
  }
}

}  // namespace

PipelineStatus PipelineRunner::RunStage(const PipelineStageDef& stage,
                                        PassRun& run) {
  run.stage = &stage;

  const bool tracing = TraceEnabled();
  if (tracing) {
    trace::Line("evt", "RunStage", run.session, "stage=%s steps=%u phases=%u",
                stage.name, stage.step_count, stage.phase_count);
  }

  uint32_t step_index = 0;
  PipelineStatus stage_status = PipelineStatus::Ok;

  for (uint32_t phase_index = 0; phase_index < stage.phase_count;
       ++phase_index) {
    const PipelinePhaseDef& phase = stage.phases[phase_index];

    if (phase.conditional != nullptr && !phase.conditional(run)) {
      if (tracing) {
        trace::Line("evt", "RunStage", run.session,
                    "phase=%s SKIP (conditional)", phase.name);
      }
      step_index = phase.first_step + phase.step_count;
      continue;
    }

    for (uint32_t i = 0; i < phase.step_count; ++i, ++step_index) {
      if (step_index >= stage.step_count) break;

      const PipelineNodeId node = stage.steps[step_index];
      const PipelineStepDef* def = FindPipelineStep(node);

      // A node with no implementation is a documented placeholder: the work
      // it names is currently performed inside a coarser step. Nothing to do.
      if (def == nullptr || def->run == nullptr) {
        if (tracing) {
          trace::Line("evt", "RunStage", run.session,
                      "phase=%s step=%s placeholder", phase.name,
                      PipelineNodeName(node));
        }
        continue;
      }

      // An HDR-aware step in a stage that cannot do HDR never runs. This is
      // a property of the stage, not a failure.
      if (def->requires_hdr_aware && !stage.hdr_capable) {
        if (tracing) {
          trace::Line("evt", "RunStage", run.session,
                      "step=%s SKIP (stage not hdr_capable)", def->name);
        }
        continue;
      }

      // Prerequisite check. A missing prerequisite means the table lists a
      // step before something it depends on, which is a bug in this file
      // rather than a runtime condition. Log once — at error level so it is
      // visible without tracing enabled — and skip the step so the frame
      // degrades instead of rendering garbage.
      if (!PrerequisitesMet(*def, run)) {
        char missing[256];
        FormatMissingPrerequisites(*def, run, missing, sizeof(missing));
        char message[384];
        std::snprintf(message, sizeof(message),
                      "pipeline: step '%s' (stage %s) skipped: unmet "
                      "prerequisite(s) [%s]",
                      def->name, stage.name, missing);
        reshade::log::message(reshade::log::level::error, message);
        if (tracing) {
          trace::Line("evt", "RunStage", run.session, "step=%s SKIP (requires %s)",
                      def->name, missing);
        }
        continue;
      }

      if (tracing) {
        trace::Line("evt", "RunStage", run.session, "phase=%s step=%s BEGIN",
                    phase.name, def->name);
      }

      const PipelineStatus status = def->run(run);

      if (status == PipelineStatus::Ok) {
        MarkCompleted(run, node);
      }

      if (tracing) {
        trace::Line("evt", "RunStage", run.session, "phase=%s step=%s END status=%d",
                    phase.name, def->name, static_cast<int>(status));
      }

      if (status != PipelineStatus::Ok) {
        // SkipFrame and Fail both stop the stage. They differ in how the
        // session is left: SkipFrame means the pass is still healthy (the
        // feature was just rebuilt, the codec is still compiling), Fail means
        // the session has been marked failed by the step itself.
        stage_status = status;
        break;
      }
    }

    if (stage_status != PipelineStatus::Ok) break;
  }

  if (tracing) {
    trace::Line("evt", "RunStage", run.session, "stage=%s DONE status=%d",
                stage.name, static_cast<int>(stage_status));
  }

  return stage_status;
}

}  // namespace reshade_dlssnr

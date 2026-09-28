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
    "SkinDetect",
    "HdrBeginFrame",
    "HistogramZero",
    "HistogramBuild",
    "HistogramReadback",
    "ResolveModelInput",
    "BuildHdrUniforms",
    "TonemapForward",
    "PrepareModelInput",
    "GradeModelInput",
    "EnsureFeature",
    "SnapshotGuides",
    "FillEvaluateParams",
    "EvaluateFeature",
    "ModelOutputBarrier",
    "ComposeModelOutput",
    "ComposeModelOutputHdr",
    "GradeModelOutput",
    "RenderDebugView",
    "WriteToBackbuffer",
    "WriteToGameResource",
    "SubmitAndWait",
    "GradeBackbuffer",
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
//   addon_controller.cpp - the HDR lifecycle and backbuffer write
//
// They publish their step definitions through SessionStepFor / HdrStepFor /
// DebugStepFor / ControllerStepFor (declared in pipeline_graph.h). Keeping the
// lookup here means the frame's shape is readable in one file, while the
// bodies stay next to the state they touch.
//
// The lookup is indexed by node id rather than scanning the four registries.
// The cost of the scans was never the point — at ~120 lookups per frame the
// difference is unmeasurable. What the index buys is a place to notice that
// two registries claim the same node: the scans took the first match and the
// second silently never ran, which is the same shape of defect as the
// mis-flagged steps this file already guards against.
//
// Built once on first use. Single-threaded: the registries are constexpr data
// with static lifetime, and the addon's frame work is serialised behind the
// controller's mutex, so there is no race to lose.

const PipelineStepDef* const* StepIndex() {
  static const PipelineStepDef* index[kNodeNameCount + 1] = {};
  // Which registry supplied each entry, so a duplicate can name the winner
  // rather than just reporting that a collision happened.
  static const char* owner[kNodeNameCount + 1] = {};
  static bool built = false;
  if (built) return index;

  built = true;

  // Sources, in the order FindPipelineStep used to consult them. The order is
  // load bearing: on a duplicate the first registry wins, exactly as the
  // previous if-chain did, so the resolution does not change.
  struct Source {
    const char* name;
    const PipelineStepDef* (*lookup)(PipelineNodeId);
  };
  const Source sources[] = {
      {"pass_session.cpp", &SessionStepFor},
      {"hdr_state.cpp", &HdrStepFor},
      {"skin_detect.cpp", &SkinDetectStepFor},
      {"debug_view.cpp", &DebugStepFor},
      {"addon_controller.cpp", &ControllerStepFor},
  };

  for (const Source& source : sources) {
    for (size_t i = 0; i < kNodeNameCount; ++i) {
      const PipelineNodeId id = static_cast<PipelineNodeId>(i);
      const PipelineStepDef* def = source.lookup(id);
      if (def == nullptr) continue;

      // A definition must describe the node it was found under: a registry
      // returning a mismatched record would dispatch the wrong function.
      if (def->id != id) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "pipeline: %s returned a step for %s naming %s; ignoring "
                      "it",
                      source.name, PipelineNodeName(id),
                      PipelineNodeName(def->id));
        reshade::log::message(reshade::log::level::error, message);
        continue;
      }

      if (index[i] != nullptr) {
        char message[256];
        std::snprintf(message, sizeof(message),
                      "pipeline: %s defines a step for %s, which %s already "
                      "defines; keeping %s (only one step can run)",
                      source.name, PipelineNodeName(id), owner[i], owner[i]);
        reshade::log::message(reshade::log::level::error, message);
        continue;
      }

      index[i] = def;
      owner[i] = source.name;
    }
  }

  return index;
}

const PipelineStepDef* FindPipelineStep(PipelineNodeId id) {
  if (!PipelineNodeValid(id)) return nullptr;
  return StepIndex()[static_cast<size_t>(id)];
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
// The guards check coverage and prerequisites, not relative order. Reordering
// a table is on you: nothing fails the build if a step lands in the wrong
// place.
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
    PipelineNodeId::GradeModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::GradeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToBackbuffer,
    PipelineNodeId::SubmitAndWait,        // includes CleanupFrameState
};

constexpr PipelinePhaseDef kPresentPhases[] = {
    {"setup", 0, 5, nullptr},
    {"feature", 5, 2, nullptr},
    {"snapshot", 7, 1, nullptr},
    {"input", 8, 1, nullptr},
    {"prepare", 9, 2, nullptr},
    {"evaluate", 11, 3, nullptr},
    {"compose", 14, 2, nullptr},
    {"output", 16, 3, nullptr},
};

// -- Post-DLSS stage --------------------------------------------------------
// Was: PassSession::OnPostDlssEvaluate (pass_session.cpp)
constexpr PipelineNodeId kPostDlssSteps[] = {
    PipelineNodeId::ResolveDevice,        // GetDevice + runtime.Initialize
    // EnsureHdr runs once per process (lazy), HdrBeginFrame once per frame.
    // Both gate themselves, so the per-pass loop below is harmless for them.
    //
    // Ordering here is load bearing, not cosmetic. EnsureComputeEngine must
    // precede HdrBeginFrame: HdrBeginFrame dispatches the histogram through
    // that engine, and it declares EnsureComputeEngine as a prerequisite, so
    // listing it first would make the prerequisite permanently unsatisfiable
    // and silently skip HDR measurement every frame.
    PipelineNodeId::EnsureHdr,
    PipelineNodeId::EnsureComputeEngine,  // codec.Init + IsReady
    PipelineNodeId::SkinDetect,
    PipelineNodeId::HdrBeginFrame,
    PipelineNodeId::ResolveWorkingScale,
    PipelineNodeId::EnsureFrameResources, // includes SetRetireCallback
    PipelineNodeId::SeedResourceStates,   // ClearState + SeedFrameState + output
    PipelineNodeId::SeedGuideStates,
    PipelineNodeId::ResolveModelInput,    // guide color or scratch snapshot
    PipelineNodeId::EnsureFeature,
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::BuildHdrUniforms,
    PipelineNodeId::PrepareModelInput,
    PipelineNodeId::GradeModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::GradeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToGameResource,  // includes CleanupFrameState
};

constexpr PipelinePhaseDef kPostDlssPhases[] = {
    {"setup", 0, 5, nullptr},   // ResolveDevice, EnsureHdr,
                                // EnsureComputeEngine, SkinDetect,
                                // HdrBeginFrame
    {"resources", 5, 4, nullptr},
    {"feature", 9, 3, nullptr},
    {"prepare", 12, 3, nullptr},
    {"evaluate", 15, 3, nullptr},
    {"compose", 18, 2, nullptr},
    {"output", 20, 2, nullptr},
};

// -- Release stage ----------------------------------------------------------
// Was: the tail of AddonController::OnPresent.
constexpr PipelineNodeId kReleaseSteps[] = {
    PipelineNodeId::GradeBackbuffer,
    PipelineNodeId::WriteDebugToBackbuffer,
    PipelineNodeId::SignalHdrAtPresent,
};

constexpr PipelinePhaseDef kReleasePhases[] = {
    {"present", 0, 3, nullptr},
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

// The step lists above must match the copies in pipeline_graph.h, which exist
// so that every translation unit defining a step can static_assert its own
// prerequisites against the stage it runs in. These compare element by
// element, not just by length, so the two cannot drift apart silently.
static_assert(NodeListsEqual(kPresentSteps, kPresentStepNodes),
              "kPresentSteps and kPresentStepNodes disagree - update both");
static_assert(NodeListsEqual(kPostDlssSteps, kPostDlssStepNodes),
              "kPostDlssSteps and kPostDlssStepNodes disagree - update both");
static_assert(NodeListsEqual(kReleaseSteps, kReleaseStepNodes),
              "kReleaseSteps and kReleaseStepNodes disagree - update both");

// The lists are the single source of truth for how many steps a stage has, so
// the counts the stage table publishes are derived from them rather than
// repeated by hand.
static_assert(kPresentStepCount == ArrayCount(kPresentStepNodes),
              "kPresentStepNodes is stale - update it to match kPresentSteps");
static_assert(kPostDlssStepCount == ArrayCount(kPostDlssStepNodes),
              "kPostDlssStepNodes is stale - update it to match kPostDlssSteps");
static_assert(kReleaseStepCount == ArrayCount(kReleaseStepNodes),
              "kReleaseStepNodes is stale - update it to match kReleaseSteps");

// Prerequisite validation lives in the translation unit that owns each step
// table, because a definition table is `constexpr` inside its anonymous
// namespace and cannot be reached from here. Each of pass_session.cpp,
// hdr_state.cpp and addon_controller.cpp asserts its own steps against the
// stage lists published in pipeline_graph.h.

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
        // Entered from OnPostDlssEvaluate: the game's command list and output
        // resource are supplied, the swapchain and queue are not.
        /*required_inputs=*/StageInput::CommandList | StageInput::OutputResource |
            StageInput::Stage,
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
        // Entered from OnPresent: the swapchain and queue are supplied, the
        // game's command list and output resource are not. Device and
        // backbuffer are produced by this stage's own steps, not passed in.
        /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue |
            StageInput::Stage,
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
        // Entered from RunSingleStage, which supplies the queue and swapchain
        // only. The stage owns a command list but opens it on demand rather
        // than being handed one, so it is not a declared input — the same
        // shape as Present, which produces run.list from its own session.
        // The release work is a backbuffer write and the HDR present signal,
        // both of which need the queue and the swapchain.
        /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue |
            StageInput::Stage,
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

// True when the run actually carries `input`.
//
// This mirrors the frame-input fields of PassRun. It deliberately reads the
// live values rather than trusting the stage declaration: the declaration
// states what the call site promises, and this is what verifies the promise
// was kept. A stage that declares an input its caller does not supply fails
// here instead of null-dereferencing inside a step.
bool InputPresent(StageInput input, const PassRun& run) {
  switch (input) {
    case StageInput::CommandList:
      return run.list != nullptr;
    case StageInput::Device:
      return run.device != nullptr;
    case StageInput::Swapchain:
      return run.swapchain != nullptr;
    case StageInput::CommandQueue:
      return run.command_queue != nullptr;
    case StageInput::OutputResource:
      return run.output_resource != nullptr;
    case StageInput::Backbuffer:
      return run.backbuffer != nullptr;
    case StageInput::Stage:
      return run.stage != nullptr;
    case StageInput::None:
      return true;
  }
  return true;
}

// Names the inputs in `set` that `run` does not carry.
const char* InputName(StageInput input) {
  switch (input) {
    case StageInput::CommandList: return "CommandList";
    case StageInput::Device: return "Device";
    case StageInput::Swapchain: return "Swapchain";
    case StageInput::CommandQueue: return "CommandQueue";
    case StageInput::OutputResource: return "OutputResource";
    case StageInput::Backbuffer: return "Backbuffer";
    case StageInput::Stage: return "Stage";
    case StageInput::None: return "None";
  }
  return "?";
}

void FormatMissingInputs(StageInput required, const PassRun& run, char* out,
                         size_t out_size) {
  out[0] = '\0';
  size_t used = 0;
  // Individual bits, walked as a fixed list so the set can grow without this
  // function needing to change shape.
  const StageInput all[] = {
      StageInput::CommandList, StageInput::Device,     StageInput::Swapchain,
      StageInput::CommandQueue, StageInput::OutputResource,
      StageInput::Backbuffer,  StageInput::Stage,
  };
  for (StageInput input : all) {
    if (!HasInput(required, input) || InputPresent(input, run)) continue;
    const int n = std::snprintf(out + used, out_size - used, "%s%s",
                                used == 0 ? "" : ",", InputName(input));
    if (n <= 0) break;
    used += static_cast<size_t>(n);
    if (used >= out_size) break;
  }
}

// True when the run supplies everything `stage` declares it requires.
bool StageInputsMet(const PipelineStageDef& stage, const PassRun& run) {
  const StageInput all[] = {
      StageInput::CommandList, StageInput::Device,     StageInput::Swapchain,
      StageInput::CommandQueue, StageInput::OutputResource,
      StageInput::Backbuffer,  StageInput::Stage,
  };
  for (StageInput input : all) {
    if (HasInput(stage.required_inputs, input) && !InputPresent(input, run)) {
      return false;
    }
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

// Remembers which (stage, step) skips have already been reported, so a skip
// that repeats every frame prints once and a skip that clears then returns
// prints again.
//
// The skip diagnostics are the runner's only per-step logging, and both fire
// every frame in exactly the situations the user is looking at: a
// mis-flagged step skipped in the wrong stage, or a prerequisite that can
// never be met. Printing them per frame turns the present path into a log
// flood, which shows up as stutter and buries the first occurrence — the one
// that matters — under thousands of identical copies. Keying on the skip
// *state* keeps the transition visible and the steady state silent.
//
// Bounded and fixed-size: the pipeline has a compile-time step count, so a
// small table of pending skips is enough, and a full table cannot lose a
// diagnostic (it simply re-prints, which is the safe failure direction).
struct SkipLogEntry {
  const void* session = nullptr;
  const char* stage = nullptr;
  PipelineNodeId node = PipelineNodeId::Idle;
  // Frame on which this skip was last seen, so entries that stop happening
  // are dropped and a recurrence logs afresh.
  uint64_t last_seen_frame = 0;
};

constexpr size_t kMaxSkipLogEntries = 64;
SkipLogEntry g_skip_log[kMaxSkipLogEntries] = {};
size_t g_skip_log_count = 0;
uint64_t g_skip_log_frame = 0;

// Advances the frame counter and drops entries not seen in the previous frame.
void BeginSkipLogFrameImpl() {
  ++g_skip_log_frame;
  size_t write = 0;
  for (size_t i = 0; i < g_skip_log_count; ++i) {
    if (g_skip_log[i].last_seen_frame + 1 >= g_skip_log_frame) {
      g_skip_log[write++] = g_skip_log[i];
    }
  }
  g_skip_log_count = write;
}

// Returns true when this skip has not been reported in the current run of
// frames, recording it as seen.
bool ShouldLogSkip(const void* session, const char* stage, PipelineNodeId node) {
  for (size_t i = 0; i < g_skip_log_count; ++i) {
    SkipLogEntry& e = g_skip_log[i];
    if (e.session == session && e.stage == stage && e.node == node) {
      e.last_seen_frame = g_skip_log_frame;
      return false;
    }
  }
  if (g_skip_log_count < kMaxSkipLogEntries) {
    g_skip_log[g_skip_log_count++] =
        SkipLogEntry{session, stage, node, g_skip_log_frame};
  } else {
    // Table full: fall back to no dedup rather than dropping the diagnostic.
    return true;
  }
  return true;
}

}  // namespace

void PipelineRunner::BeginSkipLogFrame() { BeginSkipLogFrameImpl(); }

PipelineStatus PipelineRunner::RunStage(const PipelineStageDef& stage,
                                        PassRun& run) {
  run.stage = &stage;

  const bool tracing = TraceEnabled();
  if (tracing) {
    trace::Line("evt", "RunStage", run.session, "stage=%s steps=%u phases=%u",
                stage.name, stage.step_count, stage.phase_count);
  }

  // Refuse to start a stage whose declared inputs the caller did not supply.
  //
  // The steps in a stage are shared with other stages, and the call sites
  // differ in what they pass — Present has a swapchain and no command list,
  // PostDLSS the reverse. Checking once here means a step that dereferences a
  // frame input cannot be reached with a null one, so a table edit that moves
  // a step into a stage whose call site does not provide what it needs fails
  // immediately and visibly instead of as a null dereference part-way through
  // the frame.
  //
  // Declared inputs are the contract; this is the enforcement. Deduplicated
  // through the skip log because a mismatch is a static property of the
  // configuration and would otherwise repeat every frame.
  if (!StageInputsMet(stage, run)) {
    char missing[128];
    FormatMissingInputs(stage.required_inputs, run, missing, sizeof(missing));
    if (ShouldLogSkip(run.session, stage.name, PipelineNodeId::Idle)) {
      char message[384];
      std::snprintf(message, sizeof(message),
                    "pipeline: stage %s did not run: caller supplied no [%s], "
                    "which the stage declares it needs",
                    stage.name, missing);
      reshade::log::message(reshade::log::level::error, message);
    }
    if (tracing) {
      trace::Line("evt", "RunStage", run.session, "stage=%s ABORT missing=%s",
                  stage.name, missing);
    }
    return PipelineStatus::SkipFrame;
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

      // A step that cannot work without the HDR path never runs in a stage
      // that is not HDR-capable. This is a property of the stage, not a
      // failure — but it is logged at info level rather than left to the
      // trace, because a silent skip here once hid a real bug (two steps
      // mis-flagged as HDR-only were skipped in Present, so the model was
      // evaluated with null inputs). A skip that changes behaviour should be
      // visible without opting into tracing.
      if (def->requires_hdr_aware && !stage.hdr_capable) {
        if (ShouldLogSkip(run.session, stage.name, node)) {
          char message[256];
          std::snprintf(message, sizeof(message),
                        "pipeline: step '%s' (stage %s) skipped: stage is not "
                        "HDR-capable",
                        def->name, stage.name);
          reshade::log::message(reshade::log::level::info, message);
        }
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
        // Logged once per (pass, stage, step) rather than once per frame: a
        // prerequisite that can never be met is a permanent condition, and
        // repeating it every frame is what buried the original report of it.
        if (ShouldLogSkip(run.session, stage.name, node)) {
          char message[384];
          std::snprintf(message, sizeof(message),
                        "pipeline: step '%s' (stage %s) skipped: unmet "
                        "prerequisite(s) [%s]",
                        def->name, stage.name, missing);
          reshade::log::message(reshade::log::level::error, message);
        }
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

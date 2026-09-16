// Data-driven description of the frame pipeline.
//
// The frame is a sequence of STAGES. Each stage is an ordered list of STEPS,
// grouped into PHASES. A step is a named function with declared prerequisites;
// the runner in this file executes a stage by walking its step list, checking
// prerequisites, and interpreting the returned PipelineStatus.
//
// The point of the split:
//
//   - WHAT runs, and IN WHAT ORDER  -> the tables at the bottom of this file.
//     Adding, removing or reordering a step or a stage is a one-line edit.
//   - HOW a step runs               -> the step functions, which live in the
//     translation unit that owns the work (pass_session.cpp, hdr_state.cpp,
//     debug_view.cpp) and are published here via kPipelineSteps[].
//
// The two entry points in AddonController are no longer parallel copies of the
// same loop; they both call RunStage() with a different stage definition.
//
// Fidelity note: this graph is a description of the EXISTING pipeline, not a
// redesign. Step order, barrier order, dispatch order, and submit points all
// match the pre-refactor code exactly. Any deliberate divergence is called out
// at the step that diverges.
#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "debug_view.h"
#include "dlss_guides.h"
#include "model_pipeline.h"

namespace reshade_dlssnr {

class CommandContext;
class ComputeEngine;
class FeatureLifecycle;
class HdrState;
class NgxRuntime;
class PassResources;
class PassSession;
struct FrameDebugContext;
struct PassSettings;
struct PipelineStageDef;
struct Settings;

// ---------------------------------------------------------------------------
// Node identity
// ---------------------------------------------------------------------------
//
// Flat, and ordered by increasing depth in the pipeline. The ordering is load
// bearing: a step may only declare prerequisites that appear EARLIER in this
// enum, which makes a cyclic or forward dependency impossible to express.
//
// Where a node dispatches a registered compute pass, its name matches the
// string in kComputePasses[] exactly so trace logs line up with the PSO cache.
enum class PipelineNodeId : uint32_t {
  Idle = 0,

  // -- frame / pass setup ---------------------------------------------------
  AcquireBackbuffer = 1,   // Present stage only
  ResolveDevice = 2,       // resolves the device; also runtime.Initialize
  EnsureComputeEngine = 3,
  ResetDebugContext = 4,   // restores the incremental debug state per pass
  ResolveWorkingScale = 5, // defines work_width / work_height for the pass
  EnsureFrameResources = 6,
  SeedResourceStates = 7,  // registers known frame-boundary states
  // Seeds the guide resources' tracked state. Separate from
  // SeedResourceStates because the two stages run it at different points:
  // Present seeds guides after the feature is built, PostDLSS before.
  SeedGuideStates = 8,

  // -- HDR measurement (PostDLSS stage only, first pass in the chain) -------
  // Lazily creates the HDR histogram resources and starts the compute engine
  // on the first Post-DLSS frame. Distinct from HdrBeginFrame because it runs
  // once per process, not once per frame, and must succeed before BeginFrame
  // can touch the histogram buffer.
  EnsureHdr = 9,
  HdrBeginFrame = 10,
  HistogramZero = 11,      // dispatches "ZeroHistogram"
  HistogramBuild = 12,     // dispatches "BuildHistogram"
  HistogramReadback = 13,  // leaves histogram in COPY_SOURCE for the readback

  // -- model input ----------------------------------------------------------
  ResolveModelInput = 14,  // first pass -> guide color; else scratch snapshot
  BuildHdrUniforms = 15,
  TonemapForward = 16,     // dispatches "TonemapForward"
  PrepareModelInput = 17,  // dispatches "PrepareModelInput"

  // -- model evaluation -----------------------------------------------------
  EnsureFeature = 18,
  // Re-reads the guide state. Must follow EnsureFeature: creating the NGX
  // feature can make the game re-capture its DLSS guides, so a snapshot taken
  // before the feature exists can be stale — or entirely invalid on the first
  // frame. Both stages take the snapshot at exactly this point.
  SnapshotGuides = 19,
  FillEvaluateParams = 20,
  EvaluateFeature = 21,
  ModelOutputBarrier = 22, // UavBarrier on the model output

  // -- composition ----------------------------------------------------------
  ComposeModelOutput = 23,    // dispatches "ComposeModelOutput"
  ComposeModelOutputHdr = 24, // dispatches "ComposeModelOutputHdr"

  // -- output ---------------------------------------------------------------
  RenderDebugView = 25,      // dispatches "DebugDisplay"
  WriteToBackbuffer = 26,
  WriteToGameResource = 27,
  SubmitAndWait = 28,        // Present stage only

  // -- release stage --------------------------------------------------------
  WriteDebugToBackbuffer = 29, // dispatches "DebugDisplay"
  SignalHdrAtPresent = 30,

  Count
};

const char* PipelineNodeName(PipelineNodeId id);
bool PipelineNodeValid(PipelineNodeId id);

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

// How the runner should treat a step that reports failure.
enum class FailurePolicy {
  // Mark the pass failed and stop running the stage. The pre-refactor
  // Present path used this for nearly everything.
  Fail,
  // Note it and stop this stage, but leave the pass healthy. The
  // pre-refactor PostDLSS path used this for device acquisition and for
  // runtime.Initialize, which can legitimately fail on frames where the game
  // has not loaded the DLSS model yet.
  Transient,
  // Ignore the result entirely and keep going.
  Ignore,
};

enum class PipelineStatus {
  Ok,
  // The step decided the rest of this stage should not run this frame, and
  // the reason is not an error. Distinct from Fail so the runner can skip
  // without marking the session failed.
  SkipFrame,
  Fail,
};

// ---------------------------------------------------------------------------
// Run state
// ---------------------------------------------------------------------------

// Everything a step needs, resolved once per pass and mutated as steps run.
// This is the "what does this pass depend on" answer: if a step needs
// something, it is either a field here or reachable from one.
//
// Every pointer is borrowed, never owned. Lifetimes are the caller's.
struct PassRun {
  // -- stage being executed -------------------------------------------------
  const PipelineStageDef* stage = nullptr;

  // -- owners ---------------------------------------------------------------
  PassSession* session = nullptr;
  CommandContext* cmd = nullptr;
  PassResources* resources = nullptr;
  FeatureLifecycle* feature = nullptr;
  ComputeEngine* codec = nullptr;
  NgxRuntime* runtime = nullptr;
  HdrState* hdr = nullptr;                 // null in the Present stage
  // Scratch command context the HDR histogram records through. Owned by the
  // controller; null when the stage never touches HDR.
  CommandContext* hdr_cmd = nullptr;
  FrameDebugContext* frame_debug = nullptr;

  // -- settings -------------------------------------------------------------
  const Settings* settings = nullptr;
  const PassSettings* pass = nullptr;
  uint32_t pass_index = 0;

  // -- frame inputs ---------------------------------------------------------
  ID3D12GraphicsCommandList* list = nullptr;  // the list this pass records on
  ID3D12Device* device = nullptr;
  ID3D12Resource* backbuffer = nullptr;       // Present stage only
  ID3D12Resource* output_resource = nullptr;  // PostDLSS stage only
  IDXGISwapChain* swapchain = nullptr;        // Present stage only
  ID3D12CommandQueue* command_queue = nullptr; // Present stage only

  // Present only: the swapchain's IDXGISwapChain3 interface, kept alive for
  // the duration of the stage. The steps that need it (the backbuffer
  // acquire) borrow it rather than re-querying.
  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;

  DlssGuideState guides = {};
  bool guides_valid = false;
  bool is_first_pass = false;

  // -- frame geometry -------------------------------------------------------
  // `desc` describes the image the pass writes at full resolution: the
  // backbuffer for Present, the game's output resource for PostDLSS. Keeping
  // it on the run is what lets the two stages share one set of steps.
  D3D12_RESOURCE_DESC desc = {};
  unsigned int width = 0;
  unsigned int height = 0;
  unsigned int work_width = 0;
  unsigned int work_height = 0;

  // -- produced by steps, consumed by later steps ---------------------------
  ID3D12Resource* model_input = nullptr;
  ID3D12Resource* model_color = nullptr;
  ID3D12Resource* model_output = nullptr;
  unsigned int eval_width = 0;
  unsigned int eval_height = 0;

  // Result of the NGX call. Only the Present stage consults it, and only
  // after the frame has been submitted.
  //
  // Note NVSDK_NGX_Result_Success is 0x1, NOT 0, so zero is a sentinel meaning
  // "EvaluateFeature was never reached this frame" rather than a success. The
  // check in StepSubmitAndWait treats anything other than Success as a failure,
  // which is correct either way, but the sentinel is why a step that bailed out
  // early cannot be mistaken for a clean evaluation.
  int eval_result = 0;

  // HDR tonemap uniforms, built by BuildHdrUniforms and consumed by the
  // PrepareModelInput / ComposeModelOutput steps. `hdr_uniforms_ready` false
  // means the SDR variant of each pass is selected, which is what the
  // pre-refactor `hdr_ptr == nullptr` test expressed.
  model_pipeline::HdrUniforms hdr_uniforms = {};
  bool hdr_uniforms_ready = false;

  // Filled from the pass's live resources before the debug step runs.
  DebugSnapshot debug = {};

  // -- bookkeeping ----------------------------------------------------------
  // Bitmask over PipelineNodeId, set by the runner after each successful step.
  uint64_t completed = 0;
  // Set when a step takes a branch worth seeing in the trace (first pass vs
  // chained pass, HDR vs SDR, and so on).
  const char* skip_reason = nullptr;
};

using PipelineStepFn = PipelineStatus (*)(PassRun& run);

struct PipelineStepDef {
  PipelineNodeId id;
  const char* name;
  PipelineStepFn run;

  // Prerequisites, as node ids. The runner checks these before calling `run`
  // and treats a missing one as a programmer error: it logs once and skips
  // the step rather than producing a half-built frame.
  //
  // Terminated by PipelineNodeId::Count.
  //
  // Named `depends_on` rather than `requires`: `requires` is a C++20 keyword
  // (it introduces a requires-clause) and cannot be used as a member name.
  const PipelineNodeId* depends_on;

  // Set when this step cannot do its work without the HDR path, i.e. it reads
  // `PassRun::hdr` and has no meaningful SDR behaviour. The runner consults it
  // together with the stage's hdr_capable flag and skips the step in a stage
  // that is not HDR-capable.
  //
  // It does NOT mean "this step has an HDR variant". PrepareModelInput and
  // ComposeModelOutput each select an HDR shader when `hdr_uniforms_ready` is
  // set and fall back to the SDR shader otherwise, so they run in both kinds of
  // stage. Flagging them with this was a real bug: Present is not HDR-capable,
  // so both were skipped and the model was evaluated with null inputs, which
  // NGX reported as FAIL_InvalidParameter.
  bool requires_hdr_aware = false;
};

// ---------------------------------------------------------------------------
// Phases and stages
// ---------------------------------------------------------------------------

// A phase is a contiguous run of steps that share a gate. `conditional` is
// evaluated once, before the phase's first step; a false result skips the
// whole phase. This is how "only the active pass renders the debug view" and
// "only run the HDR histogram when HDR is on" are expressed, without
// interleaving those checks into the step bodies.
struct PipelinePhaseDef {
  const char* name;
  uint32_t first_step;  // index into PipelineStageDef::steps
  uint32_t step_count;
  bool (*conditional)(const PassRun&);  // nullptr = always run
};

struct PipelineStageDef {
  const char* name;

  // The pass's `injection_stage` this stage serves. Present maps to
  // InjectionStage::Present, PostDLSS to InjectionStage::PostDLSS. A stage
  // with no mapping (e.g. the release stage) sets has_injection_stage=false.
  bool has_injection_stage = true;
  bool is_post_dlss = false;

  // Ordered steps. One line here is the entire cost of adding, removing or
  // reordering a pipeline step.
  const PipelineNodeId* steps;
  uint32_t step_count;

  const PipelinePhaseDef* phases;
  uint32_t phase_count;

  // How a failure inside this stage is treated by default. Individual steps
  // may override via their own logic; see FailurePolicy.
  FailurePolicy default_failure = FailurePolicy::Fail;

  // True when the stage owns its command list (Reset/Close/Execute/Wait).
  // The PostDLSS stage records onto the game's list and must never submit.
  bool owns_submit = false;

  // True when the stage may use the HDR path (tonemap + HDR compose).
  bool hdr_capable = false;

  // True when the stage consumes a captured DLSS output as its input, rather
  // than a backbuffer. Selects the input-resolution source and whether the
  // game's output resource must be restored afterwards.
  bool reads_dlss_output = false;

  // Ceiling on how many passes of this stage may run in one frame.
  uint32_t max_passes = 1;
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// Returns the step definition for a node, or nullptr when the node has no
// implementation (a placeholder node, e.g. one documenting a sub-step that a
// coarser step currently performs).
const PipelineStepDef* FindPipelineStep(PipelineNodeId id);

// ---------------------------------------------------------------------------
// Compile-time validation of step prerequisite declarations
// ---------------------------------------------------------------------------
//
// A step declares prerequisites as node ids, and the runner refuses to run a
// step whose prerequisites have not completed. That check is only meaningful
// if a prerequisite can actually complete BEFORE the step runs, which (since
// the runner walks its stage's step list in order) means the prerequisite must
// appear EARLIER in that list.
//
// Getting this wrong does not fail to compile and does not crash. The runner
// logs at error level and skips the step, every frame, forever — which is how
// a mis-ordered HdrBeginFrame silently disabled HDR measurement entirely.
// These helpers turn that into a build error.
//
// Usage, in the translation unit that defines a step table:
//
//   static_assert(PrerequisitesPrecedeSteps(kStageNodes, ArrayCount(kStageNodes),
//                                           kMySteps, ArrayCount(kMySteps)),
//                 "a step declares a prerequisite that runs after it");
//
// `steps` is the stage's ordered node list, published below as
// kPresentStepNodes / kPostDlssStepNodes / kReleaseStepNodes; `defs` is the
// flat definition table in the owning .cpp. A definition whose node is not in
// `steps` is skipped, because the same definition may serve a stage that lists
// it elsewhere — the histogram placeholders are in no stage list at all, with
// the work folded into HdrBeginFrame.

// True when `needle` is present in the first `limit` entries of `steps`.
constexpr bool NodePrecedes(const PipelineNodeId* steps, size_t step_count,
                            size_t limit, PipelineNodeId needle) {
  return limit == 0
             ? false
             : (steps[limit - 1] == needle ||
                NodePrecedes(steps, step_count, limit - 1, needle));
}

// Position of `needle` in `steps`, written to `*out_index`; false when absent.
//
// Absence is reported through the return value rather than a sentinel index,
// because index 0 is a legal position: folding "not found" into 0 made every
// absent node look like the stage's first step, which in turn made every
// prerequisite of an absent step look violated. The histogram placeholder
// steps are absent from the Present list and hit exactly this.
constexpr bool IndexOfNode(const PipelineNodeId* steps, size_t step_count,
                           PipelineNodeId needle, size_t* out_index) {
  return step_count == 0
             ? false
             : (steps[step_count - 1] == needle
                    ? (*out_index = step_count - 1, true)
                    : IndexOfNode(steps, step_count - 1, needle, out_index));
}

// True when every prerequisite listed by `def` appears before position
// `def_index` in `steps`. `dep_index` walks the null-terminated list.
constexpr bool StepPrerequisitesPrecedeImpl(const PipelineStepDef* def,
                                            const PipelineNodeId* steps,
                                            size_t step_count,
                                            size_t def_index,
                                            size_t dep_index) {
  return def->depends_on[dep_index] == PipelineNodeId::Count
             ? true
             : (NodePrecedes(steps, step_count, def_index,
                             def->depends_on[dep_index]) &&
                StepPrerequisitesPrecedeImpl(def, steps, step_count, def_index,
                                             dep_index + 1));
}

// True when every step in `defs` has all its prerequisites satisfied by an
// earlier entry in `steps`. Steps whose node is absent from `steps` pass
// trivially, because the same definition may serve a stage that lists it
// elsewhere (or not at all) — that is exactly the histogram placeholders'
// situation against the Present list.
//
// Note the gap this cannot close: a prerequisite that appears in NO stage list
// is accepted here, since "absent" is treated as "not this stage's business".
// The runner still refuses such a step at runtime and logs it. This check
// covers ordering, which is the mistake that is easy to make and hard to see.
constexpr bool StepPrerequisitesPrecede(const PipelineStepDef* def,
                                        const PipelineNodeId* steps,
                                        size_t step_count) {
  size_t def_index = 0;
  return def->depends_on == nullptr
             ? true
             : (IndexOfNode(steps, step_count, def->id, &def_index)
                    ? StepPrerequisitesPrecedeImpl(def, steps, step_count,
                                                   def_index, 0)
                    : true);
}

// Walks `defs` from the end so the recursion stays measurable by the compiler.
constexpr bool PrerequisitesPrecedeSteps(const PipelineNodeId* steps,
                                         size_t step_count,
                                         const PipelineStepDef* defs,
                                         size_t def_count) {
  return def_count == 0
             ? true
             : (StepPrerequisitesPrecede(defs + def_count - 1, steps,
                                         step_count) &&
                PrerequisitesPrecedeSteps(steps, step_count, defs,
                                          def_count - 1));
}

// Count of entries in a constexpr array, for use in the static_asserts below.
template <typename T, size_t N>
constexpr size_t ArrayCount(const T (&)[N]) { return N; }

// True when both arrays hold the same node ids in the same order. Used to
// prove the stage node lists here match the step tables in
// pipeline_graph.cpp, so the two cannot drift apart silently.
template <size_t N>
constexpr bool NodeListsEqual(const PipelineNodeId (&a)[N],
                              const PipelineNodeId (&b)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// True when no step in `defs` that is listed in `steps` is marked
// `requires_hdr_aware`.
//
// Use this to assert that a stage which is not HDR-capable carries no
// HDR-only step. The runner silently skips such a step, so the mistake shows up
// as a step that never runs rather than as a compile or runtime error — which
// is exactly how PrepareModelInput and ComposeModelOutput came to be skipped in
// the Present stage, leaving the model to be evaluated with null inputs.
//
// Steps not listed in `steps` are ignored, mirroring PrerequisitesPrecedeSteps.
constexpr bool NoHdrAwareSteps(const PipelineNodeId* steps, size_t step_count,
                               const PipelineStepDef* defs, size_t def_count) {
  size_t index = 0;
  return def_count == 0
             ? true
             : ((defs[def_count - 1].requires_hdr_aware &&
                 IndexOfNode(steps, step_count, defs[def_count - 1].id, &index))
                    ? false
                    : NoHdrAwareSteps(steps, step_count, defs,
                                      def_count - 1));
}

// ---------------------------------------------------------------------------
// Stage node lists
// ---------------------------------------------------------------------------
//
// The ordered node list for each stage. These live in the header, rather than
// beside the phase tables in pipeline_graph.cpp, so that a translation unit
// defining a step can static_assert its own prerequisites against them. See
// StepPrerequisitesPrecede below for why that matters.
//
// Keep in step with the step tables in pipeline_graph.cpp; the phase tiling
// assertions there are what catch a mismatch.

inline constexpr PipelineNodeId kPresentStepNodes[] = {
    PipelineNodeId::AcquireBackbuffer,
    PipelineNodeId::ResolveDevice,
    PipelineNodeId::ResolveWorkingScale,
    PipelineNodeId::EnsureFrameResources,
    PipelineNodeId::SeedResourceStates,
    PipelineNodeId::EnsureFeature,
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::EnsureComputeEngine,
    PipelineNodeId::ResolveModelInput,
    PipelineNodeId::PrepareModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToBackbuffer,
    PipelineNodeId::SubmitAndWait,
};

inline constexpr PipelineNodeId kPostDlssStepNodes[] = {
    PipelineNodeId::ResolveDevice,
    PipelineNodeId::EnsureHdr,
    PipelineNodeId::EnsureComputeEngine,
    PipelineNodeId::HdrBeginFrame,
    PipelineNodeId::ResolveWorkingScale,
    PipelineNodeId::EnsureFrameResources,
    PipelineNodeId::SeedResourceStates,
    PipelineNodeId::SeedGuideStates,
    PipelineNodeId::ResolveModelInput,
    PipelineNodeId::EnsureFeature,
    PipelineNodeId::SnapshotGuides,
    PipelineNodeId::BuildHdrUniforms,
    PipelineNodeId::PrepareModelInput,
    PipelineNodeId::FillEvaluateParams,
    PipelineNodeId::EvaluateFeature,
    PipelineNodeId::ModelOutputBarrier,
    PipelineNodeId::ComposeModelOutput,
    PipelineNodeId::RenderDebugView,
    PipelineNodeId::WriteToGameResource,
};

inline constexpr PipelineNodeId kReleaseStepNodes[] = {
    PipelineNodeId::WriteDebugToBackbuffer,
    PipelineNodeId::SignalHdrAtPresent,
};

// Per-translation-unit step registries. Each owning .cpp implements the ones
// covering the nodes whose bodies live there; returning nullptr for a node it
// does not own is required. FindPipelineStep() consults them in order.
const PipelineStepDef* SessionStepFor(PipelineNodeId id);
const PipelineStepDef* HdrStepFor(PipelineNodeId id);
const PipelineStepDef* DebugStepFor(PipelineNodeId id);
const PipelineStepDef* ControllerStepFor(PipelineNodeId id);

// The stage table. This is the single place the frame's shape is declared.
// Adding a stage, or reordering stages within the frame, is one line here.
extern const PipelineStageDef kPipelineStages[];
extern const uint32_t kPipelineStageCount;

// Looks up a stage by injection stage. Returns nullptr when no stage serves
// the requested injection point, in which case no pass runs.
const PipelineStageDef* FindPipelineStageForInjection(int injection_stage);

// The release stage: work that runs once per frame after every pass stage,
// against the swapchain rather than the game's resources.
const PipelineStageDef& PipelineReleaseStage();

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

// Executes `stage` against `run`, evaluating phase conditionals and step
// prerequisites in order. Steps that report SkipFrame or Fail stop the stage.
//
// A step is never called when its stage says it cannot run (e.g. an
// HDR-aware step in a stage that is not hdr_capable is skipped, not failed).
class PipelineRunner {
 public:
  // Returns the status of the stage as a whole: Ok when every step ran or the
  // stage stopped cleanly, the first non-Ok status otherwise.
  static PipelineStatus RunStage(const PipelineStageDef& stage, PassRun& run);

  // Marks a node complete. Steps call this via the runner; exposed for tests.
  static void MarkCompleted(PassRun& run, PipelineNodeId id);
  static bool IsCompleted(const PassRun& run, PipelineNodeId id);

  // Debug aid: when DLSSNR_TRACE is set, every step entry/exit is traced.
  static bool TraceEnabled();

  // Starts a new frame for the skip diagnostics. A step skipped by the runner
  // (unmet prerequisite, or not HDR-capable in a stage that is not) logs once
  // per occurrence rather than once per frame, because a permanent condition
  // repeated every frame floods the log and hides the first occurrence.
  // Call once per frame, from the one entry point that runs every frame.
  static void BeginSkipLogFrame();
};

}  // namespace reshade_dlssnr

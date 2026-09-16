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
  int eval_result = 0;  // NVSDK_NGX_Result; 0 == Success

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

  // Set when this step needs the HDR path. The runner consults it together
  // with the stage's hdr_capable flag before running the step.
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
};

}  // namespace reshade_dlssnr

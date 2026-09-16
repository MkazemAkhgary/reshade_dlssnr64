// Per-pass orchestrator coordinating resources, feature lifecycle, and stages.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>

#include "command_context.h"
#include "compute_engine.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "feature_lifecycle.h"
#include "frame_debug_context.h"
#include "hdr_state.h"
#include "model_pipeline.h"
#include "pass_resources.h"
#include "pipeline_graph.h"
#include "runtime.h"
#include "settings.h"
#include "status.h"

namespace reshade_dlssnr {

class PassSession {
 public:
  PassSession(DlssGuides* guides, uint32_t pass_index)
      : guides_(guides), pass_index_(pass_index) {}

  // Runs `stage` against this session. This is the single entry point the
  // AddonController uses: the session supplies the owners of its own state and
  // the controller supplies the frame inputs, and the runner walks the stage's
  // step table. Returns the stage status.
  //
  // The per-stage guards that the pre-refactor OnPresent / OnPostDlssEvaluate
  // applied before their first real work (`settings.enabled`, `failed_`, null
  // inputs) live here, so every caller gets them without repeating them.
  PipelineStatus RunStage(const PipelineStageDef& stage,
                          const Settings& settings,
                          ComputeEngine& codec,
                          NgxRuntime& runtime,
                          ID3D12GraphicsCommandList* command_list,
                          ID3D12Resource* output_resource,
                          bool is_first_pass,
                          FrameDebugContext& frame_debug,
                          HdrState* hdr,
                          CommandContext* hdr_cmd,
                          ID3D12CommandQueue* command_queue,
                          IDXGISwapChain* swapchain);

  void Shutdown(NgxRuntime& runtime);

  // Clears a previous failure and re-arms the pass for the next frame.
  // Releases the NGX feature so the retry starts from a clean handle,
  // regardless of what specifically went wrong. No-op if the pass has not
  // failed, so it is safe to call defensively.
  void Retry(NgxRuntime& runtime);

  // Snapshot of this pass's current state, for the controller's status UI.
  PassStatus GetStatus() const;

  // The list a stage records on: the session's own list for the Present
  // stage, the game's for PostDLSS. Steps that touch GPU state go through
  // this so the stage's ownership rule is stated once.
  //
  // Static because it reads only `run`; a step holds a PassSession* already,
  // but stating that it needs nothing else keeps it usable from the table.
  static ID3D12GraphicsCommandList* StageList(const PassRun& run,
                                              CommandContext& command) {
    return run.stage != nullptr && !run.stage->is_post_dlss
               ? command.GetCommandList()
               : run.list;
  }

  // -- Step bodies ----------------------------------------------------------
  //
  // Published through SessionStepFor(). Static so they convert to the
  // PipelineStepFn pointer type while still reaching this session's state.
  static PipelineStatus StepAcquireBackbuffer(PassRun& run);
  static PipelineStatus StepResolveDevice(PassRun& run);
  static PipelineStatus StepEnsureComputeEngine(PassRun& run);
  static PipelineStatus StepResolveWorkingScale(PassRun& run);
  static PipelineStatus StepEnsureFrameResources(PassRun& run);
  static PipelineStatus StepSeedResourceStates(PassRun& run);
  static PipelineStatus StepSeedGuideStates(PassRun& run);
  static PipelineStatus StepResolveModelInput(PassRun& run);
  static PipelineStatus StepBuildHdrUniforms(PassRun& run);
  static PipelineStatus StepEnsureFeature(PassRun& run);
  static PipelineStatus StepSnapshotGuides(PassRun& run);
  static PipelineStatus StepPrepareModelInput(PassRun& run);
  static PipelineStatus StepFillEvaluateParams(PassRun& run);
  static PipelineStatus StepEvaluateFeature(PassRun& run);
  static PipelineStatus StepModelOutputBarrier(PassRun& run);
  static PipelineStatus StepComposeModelOutput(PassRun& run);
  static PipelineStatus StepRenderDebugView(PassRun& run);
  static PipelineStatus StepWriteToBackbuffer(PassRun& run);
  static PipelineStatus StepWriteToGameResource(PassRun& run);
  static PipelineStatus StepSubmitAndWait(PassRun& run);

 private:
  // Sets failed_ and reason_, and logs the failure once. Use in place of
  // the `failed_ = true; reason_ = "...";` pattern so every failure has a
  // single visible log entry.
  void Fail(const char* reason);

  // Step-side wrapper around Fail(). A step that reports Fail has always
  // left a reason behind, which the status UI relies on. Passing nullptr
  // keeps an existing reason_ rather than blanking it.
  void FailForStep(const char* reason);

  // Fills `run` with this session's owners and the pass-level frame inputs.
  // The runner supplies the rest as steps run.
  void PrepareRun(PassRun& run,
                  const Settings& settings,
                  ComputeEngine& codec,
                  NgxRuntime& runtime,
                  ID3D12GraphicsCommandList* command_list,
                  ID3D12Resource* output_resource,
                  bool is_first_pass,
                  FrameDebugContext& frame_debug,
                  HdrState* hdr,
                  CommandContext* hdr_cmd,
                  ID3D12CommandQueue* command_queue,
                  IDXGISwapChain* swapchain);

  // Acquires the backbuffer and its descriptor for the current frame.
  bool AcquireBackbuffer(
      IDXGISwapChain* swapchain,
      ID3D12CommandQueue* command_queue,
      Microsoft::WRL::ComPtr<IDXGISwapChain3>& out_swapchain3,
      Microsoft::WRL::ComPtr<ID3D12Device>& out_device,
      Microsoft::WRL::ComPtr<ID3D12Resource>& out_backbuffer,
      D3D12_RESOURCE_DESC& out_desc);

  // Creates/resizes the per-pass output, composite, working, and scratch
  // resources required by the current frame.
  bool EnsureFrameResources(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height);

  // Fills the real NGX parameter block with tuning values, resources, and
  // subrects for the current evaluation.
  bool FillEvaluateParams(
      NgxRuntime& runtime,
      const PassSettings& pass,
      const DlssGuideState& guides,
      ID3D12Resource* model_color,
      ID3D12Resource* model_output,
      unsigned int eval_width,
      unsigned int eval_height,
      unsigned int width,
      unsigned int height);

  // Registers the known frame-boundary state of every pass-owned resource.
  // Called after ClearState() so inference never has to guess for resources
  // we own. Requires that CleanupFrameState() was called at the end of the
  // previous frame.
  void SeedFrameState(ID3D12Resource* backbuffer);

  // Returns every pass-owned resource to UNORDERED_ACCESS so the next frame's
  // SeedFrameState() is truthful. Called at the very end of each pass, before
  // SubmitAndWait().
  void CleanupFrameState(ID3D12GraphicsCommandList* command_list);

  // Closes the command list, submits it, waits for the GPU, and releases
  // retired game resources.
  void SubmitAndWait();

  // If this pass is the active one (matching Settings::active_pass) and a
  // debug view is selected, renders it into `frame_debug.output`. Runs on
  // the pass's own command list, so the source resources have their true
  // states. Returns true on success. No-op otherwise.
  bool MaybeRenderDebug(
      const Settings& settings,
      ComputeEngine& codec,
      ID3D12GraphicsCommandList* command_list,
      FrameDebugContext& frame_debug,
      const DebugSnapshot& snapshot,
      unsigned int width,
      unsigned int height,
      DXGI_FORMAT format);

  // Retires pass-owned resources whose size/format is about to change.
  // Wired to DlssGuides' retired-game-resource queue so the GPU is done
  // with them before they are released.
  static void RetireResourceCallback(ID3D12Resource* resource,
                                     void* user_data);

  // Everything the evaluate diagnostic prints, kept so the line can be
  // emitted only when it would say something new.
  //
  // The descriptor line is the one piece of the pipeline's logging that has to
  // be per-frame accurate but costs a printf and a log write every frame, which
  // shows up as stutter in a frame-limited game. None of the fields below
  // change frame to frame in a steady state, so comparing against the previous
  // frame's values keeps the diagnostic without paying for it — a resolution
  // change, a resource swap, a feature rebuild or a format change still prints.
  //
  // A failure (or a null input) forces the line out regardless, because that is
  // exactly when the descriptors are the answer; see StepEvaluateFeature.
  struct EvalLogKey {
    const char* stage = nullptr;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* output = nullptr;
    unsigned int color_width = 0;
    unsigned int color_height = 0;
    unsigned int output_width = 0;
    unsigned int output_height = 0;
    int color_format = 0;
    int output_format = 0;
    unsigned int eval_width = 0;
    unsigned int eval_height = 0;
    unsigned int frame_width = 0;
    unsigned int frame_height = 0;
    unsigned int input_width = 0;
    unsigned int input_height = 0;
    unsigned int work_width = 0;
    unsigned int work_height = 0;
    int guides_valid = 0;
    uint32_t pass_index = 0;
    void* feature_handle = nullptr;
    void* params = nullptr;
  };

  // Builds the descriptor line for the current evaluation from `key`.
  static void FormatEvalLine(const PassRun& run, const EvalLogKey& key,
                             char* line, size_t line_size);

  // Fills `key` from the current run so comparison and formatting agree.
  static void BuildEvalKey(const PassRun& run, void* feature_handle,
                           EvalLogKey* key);

  // True when any field the diagnostic prints differs.
  static bool EvalKeysDiffer(const EvalLogKey& a, const EvalLogKey& b);

  CommandContext command_;
  PassResources resources_;
  FeatureLifecycle feature_;

  DlssGuides* guides_ = nullptr;
  uint32_t pass_index_ = 0;

  ID3D12CommandQueue* command_queue_ = nullptr;

  bool failed_ = false;
  const char* reason_ = "";

  // Last values printed by the evaluate diagnostic, and whether anything has
  // been printed yet. `eval_log_key_valid_` false means the next evaluation
  // logs unconditionally (first frame of a pass, or after Shutdown).
  EvalLogKey eval_log_key_ = {};
  bool eval_log_key_valid_ = false;

  // Set by StepEvaluateFeature when it emits the descriptor line, so
  // StepSubmitAndWait can tell a fresh line was already written and does not
  // repeat it when the same frame also fails.
  bool eval_log_written_this_frame_ = false;
};

}  // namespace reshade_dlssnr

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
#include "debug/debug_view.h"
#include "dlss_guides.h"
#include "feature/pass/ui.h"
#include "model/feature_lifecycle.h"
#include "debug/frame_debug_context.h"
#include "hdr_state.h"
#include "model/model_pipeline.h"
#include "pass_resources.h"
#include "gpu_readback.h"
#include "pipeline_graph.h"
#include "model/runtime.h"
#include "state/settings.h"
#include "status.h"

namespace reshade_dlssnr {

class ImportanceMask;

class PassSession {
 public:
  explicit PassSession(DlssGuides* guides) : guides_(guides) {}

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
                          ImportanceMask* importance_mask,
                          ID3D12Resource* game_exposure_texture,
                          ID3D12CommandQueue* command_queue,
                          IDXGISwapChain* swapchain,
                          feature::pass::PassId pass_id = 0,
                          uint64_t frame_id = 0,
                          ID3D12Fence* temporal_feedback_fence = nullptr,
                          uint64_t temporal_feedback_fence_value = 0);

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
    return run.stage != nullptr && run.stage->owns_submit
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
  static PipelineStatus StepGainMapSplit(PassRun& run);
  static PipelineStatus StepEnsureFeature(PassRun& run);
  static PipelineStatus StepSnapshotGuides(PassRun& run);
  static PipelineStatus StepPrepareModelInput(PassRun& run);
  static PipelineStatus StepBuildEffectiveLocalExposure(PassRun& run);
  static PipelineStatus StepFillEvaluateParams(PassRun& run);
  static PipelineStatus StepEvaluateFeature(PassRun& run);
  static PipelineStatus StepModelOutputBarrier(PassRun& run);
  static PipelineStatus StepComposeModelOutput(PassRun& run);
  static PipelineStatus StepGradeModelInput(PassRun& run);
  static PipelineStatus StepTemporalPrepare(PassRun& run);
  static PipelineStatus StepGradeModelOutput(PassRun& run);
  static PipelineStatus StepRenderDebugView(PassRun& run);
  static PipelineStatus StepWriteToBackbuffer(PassRun& run);
  static PipelineStatus StepWriteToGameResource(PassRun& run);
  static PipelineStatus StepSubmitBackbuffer(PassRun& run);

 private:
  // Sets failed_ and reason_, and logs the failure once. Use in place of
  // the `failed_ = true; reason_ = "...";` pattern so every failure has a
  // single visible log entry.
  void Fail(const char* reason);

  // Step-side wrapper around Fail(). A step that reports Fail has always
  // left a reason behind, which the status UI relies on. Passing nullptr
  // keeps an existing reason_ rather than blanking it.
  void FailForStep(const char* reason);

  // Grades `target` in place, through the pass's grade scratch slot. A
  // disabled grade, a missing target or a missing device is a clean no-op, so
  // the two grade steps do not have to guard those themselves.
  //
  // The scratch is sized from the target's own descriptor rather than from the
  // frame geometry: CopyResource demands identical descriptors, so deriving
  // the scratch from anything else is exactly how a size or format mismatch
  // gets in.
  static PipelineStatus GradeInPlace(PassRun& run,
                                     ID3D12Resource* target,
                                     const GradeSettings& grade,
                                     uint32_t scratch_slot,
                                     bool model_sdr_domain = false,
                                     ID3D12Resource* exposure_map = nullptr);

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
                  ImportanceMask* importance_mask,
                  ID3D12Resource* game_exposure_texture,
                  ID3D12CommandQueue* command_queue,
                  IDXGISwapChain* swapchain,
                  feature::pass::PassId pass_id = 0,
                  uint64_t frame_id = 0,
                  ID3D12Fence* temporal_feedback_fence = nullptr,
                  uint64_t temporal_feedback_fence_value = 0);

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
      unsigned int work_height,
      bool allow_identity_input);

  // Fills the real NGX parameter block with tuning values, resources, and
  // subrects for the current evaluation.
  bool FillEvaluateParams(
      NgxRuntime& runtime,
      const feature::pass::PassSnapshot& pass,
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
  // SubmitBackbuffer().
  void CleanupFrameState(ID3D12GraphicsCommandList* command_list);

  // Closes and submits the command list, then signals a queue fence without
  // blocking. The allocator waits for completion only when its ring slot is reused.
  void SubmitBackbuffer();

  // If this pass is the active one (matching Settings::active_pass_id) and a
  // debug view is selected, renders it into `frame_debug.output`. Runs on
  // the pass's own command list, so the source resources have their true
  // states. Returns true on success. No-op otherwise.
  bool MaybeRenderDebug(
      int debug_view,
      feature::pass::PassId active_pass_id,
      ComputeEngine& codec,
      ID3D12GraphicsCommandList* command_list,
      FrameDebugContext& frame_debug,
      const DebugSnapshot& snapshot,
      const model_pipeline::HdrUniforms* hdr,
      unsigned int width,
      unsigned int height,
      DXGI_FORMAT format);

  // Retires pass-owned resources whose size/format is about to change.
  // Wired to DlssGuides' retired-game-resource queue so the GPU is done
  // with them before they are released.
  static void RetireResourceCallback(ID3D12Resource* resource,
                                     void* user_data);

  // The descriptor diagnostic is emitted on first use and when stable NGX
  // descriptor-contract values change (stage, dimensions, formats). Resource
  // identities, guide-valid state, feature handles, and the shared parameter
  // pointer are deliberately excluded from change detection because they may
  // vary across evaluations without changing the descriptor contract. Null
  // input transitions are reported once; persistent evaluation failures remain
  // visible through the error path without repeating this INFO line.
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

  // True when the stable stage/descriptor contract differs; transient runtime
  // identities and guide availability are intentionally ignored.
  static bool EvalKeysDiffer(const EvalLogKey& a, const EvalLogKey& b);

  CommandContext command_;
  PassResources resources_;
  FeatureLifecycle feature_;

  // Per-pass temporal amortization state. GPU history lives in PassResources;
  // only feedback samples and policy bookkeeping are CPU-side.
  GpuReadback temporal_readback_;
  bool temporal_history_valid_ = false;
  bool temporal_feedback_valid_ = false;
  float temporal_invalid_fraction_ = 1.0f;
  uint64_t temporal_last_refresh_frame_ = 0;
  uint64_t temporal_last_frame_id_ = 0;
  uint64_t temporal_last_guide_serial_ = 0;
  uint64_t temporal_settings_signature_ = 0;
  int temporal_last_mode_ = 0;
  unsigned int temporal_guide_width_ = 0;
  unsigned int temporal_guide_height_ = 0;
  unsigned int temporal_motion_width_ = 0;
  unsigned int temporal_motion_height_ = 0;
  unsigned int temporal_guide_base_x_ = 0;
  unsigned int temporal_guide_base_y_ = 0;
  unsigned int temporal_guide_subrect_width_ = 0;
  unsigned int temporal_guide_subrect_height_ = 0;
  unsigned int temporal_motion_base_x_ = 0;
  unsigned int temporal_motion_base_y_ = 0;
  unsigned int temporal_motion_subrect_width_ = 0;
  unsigned int temporal_motion_subrect_height_ = 0;
  unsigned int temporal_reference_width_ = 0;
  unsigned int temporal_reference_height_ = 0;
  DXGI_FORMAT temporal_depth_format_ = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT temporal_motion_format_ = DXGI_FORMAT_UNKNOWN;
  float temporal_mv_scale_x_ = 1.0f;
  float temporal_mv_scale_y_ = 1.0f;

  DlssGuides* guides_ = nullptr;
  // The pass's stable id, set per RunStage call. Sessions are keyed by id,
  // so this is always the id of the pass this session belongs to.
  feature::pass::PassId pass_id_ = 0;

  ID3D12CommandQueue* command_queue_ = nullptr;

  bool failed_ = false;
  const char* reason_ = "";

  // Last values printed by the evaluate diagnostic, and whether anything has
  // been printed yet. `eval_log_key_valid_` false means the next evaluation
  // logs unconditionally (first frame of a pass, or after Shutdown).
  EvalLogKey eval_log_key_ = {};
  bool eval_log_key_valid_ = false;

  // Set by StepEvaluateFeature when it emits the descriptor line, so
  // StepSubmitBackbuffer can tell a fresh line was already written and does not
  // repeat it when the same frame also fails.
  bool eval_log_written_this_frame_ = false;
};

}  // namespace reshade_dlssnr

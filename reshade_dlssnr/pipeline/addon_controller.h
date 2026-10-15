// Controller facade owning shared runtime and per-pass sessions.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "command_context.h"
#include "dlss_guides.h"
#include "debug/frame_debug_context.h"
#include "hdr_state.h"
#include "pass_session.h"
#include "pipeline_graph.h"
#include "model/runtime.h"
#include "state/settings.h"
#include "importance_mask/importance_mask.h"
#include "status.h"

namespace reshade_dlssnr {

class AddonController {
 public:
  static AddonController& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  // Runs enabled Pre-DLSS passes inline before the game's DLSS evaluation.
  // `input_resource` is NGX's Color parameter; each pass writes its result
  // back before control returns to the game's original EvaluateFeature call.
  void OnPreDlssEvaluate(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* input_resource,
      ID3D12Resource* exposure_texture);

  // Runs the enabled Post-DLSS passes inline on the game's command list,
  // right after the DLSS pass. Each pass writes its composed result back
  // into `output_resource`. The active pass (if it is a Post-DLSS pass)
  // also captures its debug view.
  void OnPostDlssEvaluate(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource,
      ID3D12Resource* exposure_texture);

  void Shutdown();

  // Thread-safe snapshot of the HDR measurement state, for the UI.
  HdrSnapshot GetHdrSnapshot() const;

  // Thread-safe snapshot of the addon's current health, for the UI.
  StatusSnapshot GetStatus() const;


  // Ensures a session exists for every pass in `order`. Removed sessions leave
  // the live map immediately but are retained until a queue fence proves their
  // previously recorded GPU work has completed. Called by the UI after changes.
  void EnsureSessions(const feature::pass::PassOrder& order);

  // Clears the failure state of every pass. One button in the UI, so the
  // user never has to click through tabs to un-stick a frame.
  void RetryAll();

  // Records why the Post-DLSS path was skipped, for display in the Status
  // section. Pass nullptr to clear — the hook calls this on every frame,
  // so the value only persists while the condition does.
  void SetPostDlssBlocked(const char* reason);

  // Copies frame_debug.output to the swapchain backbuffer. The debug image
  // was already rendered (at pass time) into a resource we own; this is
  // purely a present-time override so the game's rendering is unaffected.
  //
  // Public because it is the body of the WriteDebugToBackbuffer pipeline
  // step, which is a free function registered with the runner.
  void PresentDebugOutput(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain,
      const FrameDebugContext& debug);

  // -- The Release stage's command list -------------------------------------
  //
  // The Release stage runs after the game has submitted, so it cannot borrow
  // the game's list the way PostDLSS does, and RunSingleStage has no session
  // to own one the way Present does. These two are that list.
  //
  // BeginReleaseCommands opens it on first use in a frame and returns it;
  // later calls in the same frame hand back the same open list, so several
  // release steps record into a single submit. EndReleaseCommands closes,
  // submits, and signals the list without blocking the CPU.
  //
  // Normally an idle frame does not pay for a submit. When retired
  // resources are waiting for GPU completion, OnPresent explicitly opens this
  // list so the Release stage provides a queue-ordered lifetime fence.
  ID3D12GraphicsCommandList* BeginReleaseCommands(
      ID3D12CommandQueue* command_queue);
  void EndReleaseCommands(ID3D12CommandQueue* command_queue);

  // Grades the swapchain backbuffer in place, through the Release stage's own
  // list. Like PresentDebugOutput, this records and leaves the submit to
  // RunSingleStage.
  //
  // The backbuffer's encoding comes from the controller's own HdrState, which
  // is the same source the compose uses to encode into it. A controller that
  // never went through HDR handling reports SDR, and its backbuffer is the
  // game's own SDR image.
  //
  // The grade needs somewhere to park the source before overwriting it, and
  // the Release stage has no PassResources, so the scratch lives here.
  void GradeBackbuffer(ComputeEngine& codec,
                       ID3D12CommandQueue* command_queue,
                       IDXGISwapChain* swapchain,
                       const GradeSettings& grade);

  // HDR measurement bring-up, exposed for the EnsureHdr pipeline step. The
  // lazy-init flag spans every pass and frame, so it belongs to the
  // controller rather than to a session.
  bool HdrInitialized() const { return hdr_initialized_; }
  bool EnsureHdr(ID3D12Device* device);

  // Shared Importance Mask state. Its frame snapshot is used by HDR metering;
  // generation happens after the first successful Pre/Post-DLSS evaluation.
  ImportanceMask& GetImportanceMask() { return importance_mask_; }

 private:
  AddonController()
      : hdr_preparation_session_(&DlssGuides::Get()) {}
  ~AddonController() = default;

  AddonController(const AddonController&) = delete;
  AddonController& operator=(const AddonController&) = delete;

  // Refreshes the swapchain backbuffer format and extent. Called once per
  // ReShade Present; game-resource passes reuse the previous cached extent
  // to render diagnostics at a copy-compatible size.
  // A failure to query leaves the cached value untouched.
  void RefreshBackbufferFormat(IDXGISwapChain* swapchain);

  // Measures the current frame with the published previous-frame Importance
  // Mask before the first enabled PreDLSS or PostDLSS pass. Generation happens
  // after model evaluation. Backbuffer-only frames never call this helper.
  void PrepareFrameForPass(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* source_resource,
      ID3D12Resource* exposure_texture,
      bool pre_dlss);

  // Builds the PassRun for one pass of `stage` and hands it to the session.
  //
  // `command_list` / `output_resource` are the game's resources for PreDLSS
  // and PostDLSS, and are null for Backbuffer. `command_queue` / `swapchain`
  // are Backbuffer inputs and are null for game-resource stages. Forward the
  // correct pair for the stage; the runner rejects missing required inputs.
  PipelineStatus RunPassStage(
      const PipelineStageDef& stage,
      feature::pass::PassId pass_id,
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource,
      bool is_first_pass,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain,
      ID3D12Resource* exposure_texture = nullptr);

  // Runs a stage once, outside the per-pass loop. Used for the Release stage,
  // whose steps act on the swapchain rather than on a game resource. It is
  // also the submit point for that stage's own command list; see
  // BeginReleaseCommands.
  PipelineStatus RunSingleStage(
      const PipelineStageDef& stage,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  // Refresh status_ from the current state. Called at the end of every
  // OnPresent and OnPostDlssEvaluate.
  void RefreshStatus(const Settings& settings);

  // Passes removed from the live order must keep their NGX feature and GPU
  // resources alive until a queue-ordered Release fence proves that all earlier
  // pass work has completed. These helpers are called with g_direct_mutex held.
  bool HasUnfencedRetiredSessions() const;
  void TagRetiredSessions(uint64_t fence_value);
  void ReleaseRetiredSessionsCompleted(uint64_t completed_fence_value);

  NgxRuntime runtime_;
  // One session per live pass, keyed by the pass's stable id. Sessions are
  // created on demand and retired when a pass leaves pass_order. Keying by
  // id means a reorder never migrates a feature handle, a failure flag, or
  // an eval diagnostic between passes.
  std::unordered_map<feature::pass::PassId, std::unique_ptr<PassSession>> sessions_;
  struct RetiredPassSession {
    std::unique_ptr<PassSession> session;
    uint64_t fence_value = 0;  // zero means no queue marker has been enqueued yet
  };
  // A session removed while its commands may still be in flight. Keep the
  // entire session (including its NGX feature) until its fence completes.
  std::vector<RetiredPassSession> retired_sessions_;
  // Internal session: detection is shared across passes and must not latch a
  // failure onto a user-configured pass.
  PassSession hdr_preparation_session_;
  FrameDebugContext frame_debug_;
  // The current NGX EvaluateFeature call has already reset the debug target at
  // PreDLSS. PostDLSS must preserve it so a selected PreDLSS pass can display.
  bool pre_stage_seen_ = false;
  HdrState hdr_;
  ImportanceMask importance_mask_;

  // The Release stage's command list, opened on demand by
  // BeginReleaseCommands and submitted by EndReleaseCommands.
  CommandContext release_command_;
  uint64_t frame_id_ = 1;
  // A game-list temporal stats copy is complete only after the next Present queue marker.
  bool temporal_feedback_pending_ = false;
  // Keep the most recently observed direct queue so addon shutdown can place
  // a final queue marker before releasing sessions that may have GPU work.
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> last_command_queue_;

  // True between BeginReleaseCommands and EndReleaseCommands. Distinguishes
  // "the list is open and has work in it" from "the list was never opened",
  // which keeps an idle frame from paying a submit. Submission is asynchronous.
  bool release_list_begun_ = false;
  // Real ApplyGrade failures are logged once; startup-not-ready is not a failure.
  bool grade_backbuffer_failure_logged_ = false;

  // Ping-pong target for the backbuffer grade, sized from the backbuffer it
  // was last used with. Recreated on a size or format change; replaced
  // textures are sent through the deferred-retire queue and released after a
  // queue-ordered fence completes.
  Microsoft::WRL::ComPtr<ID3D12Resource> grade_scratch_;

  // The grade writes here, not to the backbuffer: a swapchain image has no
  // ALLOW_UNORDERED_ACCESS, and naming it as a UAV is an invalid command.
  Microsoft::WRL::ComPtr<ID3D12Resource> grade_scratch_out_;
  unsigned int grade_scratch_width_ = 0;
  unsigned int grade_scratch_height_ = 0;
  DXGI_FORMAT grade_scratch_format_ = DXGI_FORMAT_UNKNOWN;

  // Command context used only for the HDR histogram dispatch (which does
  // not submit — it just records state on the game's list).
  CommandContext pass1_command_;

  // Shared compute codec for every pass and the HDR histogram. One engine
  // means one root signature, one descriptor heap, and one background
  // compile of kComputePasses[] instead of one per session.
  ComputeEngine codec_;

  // Set once after the first successful hdr_.Ensure() + codec_.Init().
  // After that, Ensure/Init are not called again — BeginFrame does its own
  // lightweight polling and dispatch scheduling.
  bool hdr_initialized_ = false;

  // Backbuffer format from the last Present. The debug output is displayed
  // on the backbuffer, so we create it in this format. Cached because a
  // Post-DLSS pass (which runs before Present) needs to know the format
  // before we get a chance to query the backbuffer this frame.
  DXGI_FORMAT backbuffer_format_ = DXGI_FORMAT_UNKNOWN;

  // Latest status snapshot, refreshed each frame. Guarded by the same
  // mutex that guards every other controller entry point.
  StatusSnapshot status_;

  // Sticky until cleared by SetPostDlssBlocked(nullptr). Copied into the
  // status snapshot by RefreshStatus.
  bool post_dlss_blocked_ = false;
  char post_dlss_block_reason_[kStatusTextLength] = {};
};

}  // namespace reshade_dlssnr

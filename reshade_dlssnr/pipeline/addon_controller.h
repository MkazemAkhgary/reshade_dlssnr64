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
#include "gpu_resources.h"
#include "dlss_guides.h"
#include "debug/frame_debug_context.h"
#include "hdr_state.h"
#include "pass_session.h"
#include "pipeline_graph.h"
#include "model/runtime.h"
#include "state/settings.h"
#include "exposure_mask/exposure_mask.h"
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

  // Requests a full addon restart. The request is deferred until a queue-ordered
  // fence proves earlier NGX/GPU work has completed.
  void RestartAll();
  bool RestartPending() const;

  // Records why the Post-DLSS path was skipped, for display in the Status
  // section. Pass nullptr to clear - the hook calls this on every frame,
  // so the value only persists while the condition does.
  void SetPostDlssBlocked(const char* reason);

  // Same contract for the Pre-DLSS stage: nullptr clears, anything else is a
  // reason the stage cannot run in this game.
  void SetPreDlssBlocked(const char* reason);

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
  // Normally an idle frame does not pay for a submit. When retired resources
  // are waiting for GPU completion, SyncRetiredResources opens this list via
  // SignalReleaseMarker so the frame still provides a queue-ordered fence.
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

  // The Release queue's most recently signaled fence value. DlssGuides
  // uses this to tag resources it retires during CaptureGuides so that
  // an addon stall (debugger, profiler pause) cannot hold the game's
  // D3D12 resources alive past the point the game thinks they are gone.
  // 0 means no fence has been signaled yet; the Present-time tagging
  // path still covers that case.
  uint64_t ReleaseFenceHint() const;

  // Signals every live and retired pass HDR readback after game work is queued.
  void SignalHdrAtPresent(ID3D12CommandQueue* queue);
  void AdvanceMaskFrames(bool enabled);

 private:
  AddonController()
      = default;
  ~AddonController() = default;

  AddonController(const AddonController&) = delete;
  AddonController& operator=(const AddonController&) = delete;

  // Refreshes the swapchain backbuffer format and extent. Called once per
  // ReShade Present; game-resource passes reuse the previous cached extent
  // to render diagnostics at a copy-compatible size.
  // A failure to query leaves the cached value untouched.
  void RefreshBackbufferFormat(IDXGISwapChain* swapchain);

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
      ID3D12Resource* exposure_texture = nullptr,
      HdrState* previous_hdr = nullptr);

  // Runs this frame's work and returns the fence value of a queue marker that
  // was already signaled this frame, or 0 when nothing was signaled. A non-zero
  // return covers every command recorded before it, so the caller can tag and
  // free retired resources against it without submitting again.
  uint64_t RunPresentFrame(ID3D12CommandQueue* command_queue,
                           IDXGISwapChain* swapchain);

  // Opens and submits the release list purely to produce a queue marker, and
  // handles the temporal-feedback fallback when the list cannot be opened.
  // Returns 0 when no marker could be produced.
  uint64_t SignalReleaseMarker(ID3D12CommandQueue* command_queue);

  // The single place retired resources are tagged and freed. Every Present path
  // ends here.
  void SyncRetiredResources(ID3D12CommandQueue* command_queue, uint64_t marker);

  // True when something is waiting for a queue marker: an untagged retired
  // resource, a retired session with no fence, or a temporal feedback copy
  // whose predicted fence value has not been signaled yet. Entries that
  // already carry a fence are excluded - a new marker would not free them
  // any sooner, so signaling one would be a wasted submit.
  bool NeedsQueueMarker() const;

  // Releases every runtime object the addon owns: pass sessions, guide
  // references, command contexts, the compute engine, the debug image, and the
  // NGX runtime, then force-releases the retire queue.
  //
  // Both teardown paths call this and nothing else, so there is one sequence to
  // maintain. The caller must have already proven the GPU is idle - either by
  // waiting for the queue (`Shutdown`) or by observing the restart marker
  // complete (`FinishRestartAfterFence`). Must be called with g_direct_mutex
  // held.
  void TeardownRuntime();

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
  void FinishRestartAfterFence();

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
  FrameDebugContext frame_debug_;
  // The current NGX EvaluateFeature call has already reset the debug target at
  // PreDLSS. PostDLSS must preserve it so a selected PreDLSS pass can display.
  bool pre_stage_seen_ = false;

  // The Release stage's command list, opened on demand by
  // BeginReleaseCommands and submitted by EndReleaseCommands.
  CommandContext release_command_;
  uint64_t frame_id_ = 1;
  // A game-list temporal stats copy is complete only after the next Present queue marker.
  bool temporal_feedback_pending_ = false;
  // Keep the most recently observed direct queue so addon shutdown can place
  // a final queue marker before releasing sessions that may have GPU work.
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> last_command_queue_;

  // Restart requests are handled only at Present. Once the marker is queued,
  // NGX hooks are ignored until it completes and resources can be released.
  bool restart_requested_ = false;
  bool restart_waiting_for_fence_ = false;
  uint64_t restart_fence_value_ = 0;

  // True between BeginReleaseCommands and EndReleaseCommands. Distinguishes
  // "the list is open and has work in it" from "the list was never opened",
  // which keeps an idle frame from paying a submit. Submission is asynchronous.
  bool release_list_begun_ = false;
  // Real ApplyGrade failures are logged once; startup-not-ready is not a failure.
  bool grade_backbuffer_failure_logged_ = false;

  // Ping-pong target for the backbuffer grade, sized from the backbuffer it
  // was last used with. Recreated on a size or format change; replaced
  // textures go through the shared retire queue.
  OwnedTexture grade_scratch_;

  // The grade writes here, not to the backbuffer: a swapchain image has no
  // ALLOW_UNORDERED_ACCESS, and naming it as a UAV is an invalid command.
  OwnedTexture grade_scratch_out_;

  // Command context used only for the HDR histogram dispatch (which does
  // not submit - it just records state on the game's list).
  CommandContext pass1_command_;

  // Shared compute codec for every pass and the HDR histogram. One engine
  // means one root signature, one descriptor heap, and one background
  // compile of kComputePasses[] instead of one per session.
  ComputeEngine codec_;


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
  bool pre_dlss_blocked_ = false;
  char pre_dlss_block_reason_[kStatusTextLength] = {};
};

}  // namespace reshade_dlssnr

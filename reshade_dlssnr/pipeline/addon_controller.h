// Controller facade owning shared runtime and per-pass sessions.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "command_context.h"
#include "dlss_guides.h"
#include "debug/frame_debug_context.h"
#include "hdr_state.h"
#include "pass_session.h"
#include "pipeline_graph.h"
#include "model/runtime.h"
#include "state/settings.h"
#include "skin/skin_detect.h"
#include "status.h"

namespace reshade_dlssnr {

class AddonController {
 public:
  static AddonController& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  // Runs the enabled Post-DLSS passes inline on the game's command list,
  // right after the DLSS pass. Each pass writes its composed result back
  // into `output_resource`. The active pass (if it is a Post-DLSS pass)
  // also captures its debug view.
  void OnPostDlssEvaluate(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource);

  void Shutdown();

  // Thread-safe snapshot of the HDR measurement state, for the UI.
  HdrSnapshot GetHdrSnapshot() const;

  // Thread-safe snapshot of the addon's current health, for the UI.
  StatusSnapshot GetStatus() const;


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
  // submits and waits.
  //
  // A frame in which nothing records never opens the list, so it pays neither
  // the submit nor the wait — which is the point, because the wait is a full
  // GPU sync at present time and this stage runs on every frame.
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

  // The model-driven skin detection pass. Owned here rather than per-pass
  // because one mask per frame drives the shared HDR histogram.
  SkinDetect& GetSkinDetect() { return skin_detect_; }

 private:
  AddonController() {
    passes_.reserve(kMaxPasses);
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      passes_.emplace_back(&DlssGuides::Get(), i);
    }
  }
  ~AddonController() = default;

  AddonController(const AddonController&) = delete;
  AddonController& operator=(const AddonController&) = delete;

  // Refreshes backbuffer_format_ from the swapchain's current backbuffer.
  // Called once per Present, before any pass runs, so both the debug output
  // buffer and the Release stage's copy see the format actually in use.
  // A failure to query leaves the cached value untouched.
  void RefreshBackbufferFormat(IDXGISwapChain* swapchain);

  // Builds the PassRun for one pass of `stage` and hands it to the session.
  //
  // `command_list` / `output_resource` are the game's, and are null for the
  // Present stage. `command_queue` / `swapchain` are the Present stage's
  // inputs, and are null for PostDLSS. Both pairs must be forwarded: the
  // runner rejects a Present stage whose swapchain is missing, so passing
  // null here silently skips the whole stage rather than failing loudly.
  PipelineStatus RunPassStage(
      const PipelineStageDef& stage,
      uint32_t pass_index,
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource,
      bool is_first_pass,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

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

  NgxRuntime runtime_;
  std::vector<PassSession> passes_;
  FrameDebugContext frame_debug_;
  HdrState hdr_;
  SkinDetect skin_detect_;

  // The Release stage's command list, opened on demand by
  // BeginReleaseCommands and submitted by EndReleaseCommands.
  CommandContext release_command_;

  // True between BeginReleaseCommands and EndReleaseCommands. Distinguishes
  // "the list is open and has work in it" from "the list was never opened",
  // which is what keeps an idle frame from paying a submit and a GPU wait.
  bool release_list_begun_ = false;

  // Ping-pong target for the backbuffer grade, sized from the backbuffer it
  // was last used with. Recreated on a size or format change, which is safe
  // without a retire callback because the Release stage waits for its list at
  // the end of every frame: the previous frame's use cannot still be in
  // flight.
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

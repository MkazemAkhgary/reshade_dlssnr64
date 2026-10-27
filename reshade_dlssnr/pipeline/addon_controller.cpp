#include "addon_controller.h"
#include "profiling/profile.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "feature/settings_store.h"
#include "ctl/values.h"
#include "d3d12_state.h"
#include "dlss_guides.h"
#include "feature/debug_log/view.h"
#include "feature/grade/view.h"
#include "feature/hdr/view.h"
#include "feature/exposure_mask/view.h"
#include "feature/pass/settings.h"
#include "feature/pass/view.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"
#include "gpu/gpu_resources.h"
#include "profiling/tracy_integration.h"
#include "state/settings.h"
#include "streamline_compat.h"

namespace reshade_dlssnr {

namespace {
// Serializes every controller entry point and every hook that touches addon
// state. Currently the outermost lock: the guides lock and the retire queue are
// taken inside it, and neither takes it back. That one-way nesting is what makes
// the order safe; it is not a constraint on the order itself.
std::mutex g_direct_mutex;

// The drain's cost is one queue drain: the marker completes when the work
// already submitted finishes. This bound only fires on a device that has
// stopped progressing without reporting itself removed.
constexpr DWORD kDeviceSwapDrainTimeoutMs = 5000;

// Log the drain above this, so the bound's distance stays observable.
constexpr long long kDeviceSwapDrainLogMs = 50;

}  // namespace

AddonController& AddonController::Get() {
  static AddonController direct;
  return direct;
}

void AddonController::RefreshBackbufferFormat(IDXGISwapChain* swapchain) {
  Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) || sc3 == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<ID3D12Resource> bb;
  const UINT idx = sc3->GetCurrentBackBufferIndex();
  if (FAILED(sc3->GetBuffer(idx, IID_PPV_ARGS(&bb))) || bb == nullptr) return;

  const D3D12_RESOURCE_DESC desc = bb->GetDesc();
  backbuffer_format_ = desc.Format;
  frame_debug_.preferred_width = static_cast<unsigned int>(desc.Width);
  frame_debug_.preferred_height = desc.Height;
  TRACE_NAME(bb.Get(), "backbuffer");
}

PipelineStatus AddonController::RunPassStage(const PipelineStageDef& stage,
                                             feature::pass::PassId pass_id,
                                             ID3D12GraphicsCommandList* command_list,
                                             ID3D12Resource* output_resource,
                                             bool is_first_pass,
                                             ID3D12CommandQueue* command_queue,
                                             IDXGISwapChain* swapchain,
                                             ID3D12Resource* exposure_texture,
                                             HdrState* previous_hdr) {
  DLSSNR_PROFILE_RENDER_L1("AddonController::RunPassStage");
  const Settings& settings = GetSettings();

  std::unique_ptr<PassSession>& slot = sessions_[pass_id];
  if (slot == nullptr) {
    slot = std::make_unique<PassSession>(&DlssGuides::Get());
  }

  uint64_t run_frame_id = frame_id_;
  ID3D12Fence* feedback_fence = nullptr;
  uint64_t feedback_fence_value = 0;
  const ctl::ValueStore& store = feature::SettingsStore();
  const feature::pass::PassSnapshot pass_state =
      feature::pass::Read(store, feature::pass::PassKey(pass_id));
  if (pass_state.temporal_mode > 0 && !stage.owns_submit &&
      command_list != nullptr) {
    // Feedback is copied on the game's list, but completion is observed via a
    // marker signalled later on the same queue at Present. Ensure the marker
    // fence exists now, without opening or submitting the Release list here.
    Microsoft::WRL::ComPtr<ID3D12Device> queried_device;
    if (SUCCEEDED(command_list->GetDevice(IID_PPV_ARGS(&queried_device))) &&
        queried_device != nullptr) {
      Microsoft::WRL::ComPtr<ID3D12Device> native_device =
          sl_compat::Native(queried_device.Get());
      if (native_device != nullptr && release_command_.Ensure(native_device.Get())) {
        feedback_fence = release_command_.GetFence();
        feedback_fence_value = release_command_.GetNextFenceValue();
        if (feedback_fence != nullptr && feedback_fence_value != 0)
          temporal_feedback_pending_ = true;
      }
    }
  }

  return slot->RunStage(
      stage, settings, codec_, runtime_, command_list, output_resource,
      is_first_pass, frame_debug_, &slot->Hdr(), previous_hdr, &pass1_command_,
      &slot->Mask(), exposure_texture, command_queue, swapchain, pass_id,
      run_frame_id, feedback_fence, feedback_fence_value);
}

PipelineStatus AddonController::RunSingleStage(
    const PipelineStageDef& stage,
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  DLSSNR_PROFILE_RENDER_L2("AddonController::RunSingleStage");
  const Settings& settings = GetSettings();

  PassRun run;
  run.codec = &codec_;
  run.runtime = &runtime_;
  run.hdr = nullptr;
  run.frame_debug = &frame_debug_;
  run.settings = &settings;
  run.command_queue = command_queue;
  run.swapchain = swapchain;

  // Release only grades the backbuffer, presents debug output, and signals
  // outstanding per-pass HDR readbacks. None of those settings are global HDR
  // or Exposure Mask settings.
  {
    const ctl::ValueStore& store = feature::SettingsStore();
    const ctl::InstanceKey shared = ctl::InstanceKey::Shared();
    run.debug_view = feature::debug_log::Read(store, shared);
  }

  codec_.InvalidateBindings();
  const PipelineStatus status = PipelineRunner::RunStage(stage, run);

  // This is the single submit point for the Release list. OnPresent may have
  // opened it solely to fence retired resources even if no stage step records.
  EndReleaseCommands(command_queue);

  return status;
}

ID3D12GraphicsCommandList* AddonController::BeginReleaseCommands(
    ID3D12CommandQueue* command_queue) {
  if (release_list_begun_) return release_command_.GetCommandList();
  if (command_queue == nullptr) return nullptr;

  Microsoft::WRL::ComPtr<ID3D12Device> device =
      sl_compat::NativeDeviceOf(command_queue);
  if (device == nullptr) return nullptr;
  if (!release_command_.Ensure(device.Get())) return nullptr;

  // Reset and clear once per frame, so the state a step seeds is the state
  // the list starts from. A later caller in the same frame must not do this
  // again - it would discard the commands already recorded.
  release_command_.Reset();
  release_command_.ClearState();
  release_list_begun_ = true;

  TRACE_CMD(this, "begin_release_commands queue=%d",
            trace::ResourceId(command_queue));
  return release_command_.GetCommandList();
}

void AddonController::EndReleaseCommands(ID3D12CommandQueue* command_queue) {
  if (!release_list_begun_) return;
  release_list_begun_ = false;

  TRACE_CMD(this, "end_release_commands");
  release_command_.CloseAndSubmit(command_queue);
  release_command_.SignalGpu(command_queue);
}

void AddonController::GradeBackbuffer(ComputeEngine& codec,
                                      ID3D12CommandQueue* command_queue,
                                      IDXGISwapChain* swapchain,
                                      const GradeSettings& grade) {
  if (!grade.enabled) return;
  if (command_queue == nullptr || swapchain == nullptr) return;

  // The grade shader is compiled by the background worker. During startup the
  // pass can be temporarily unavailable; that is normal initialization, not a
  // failed grade. Most importantly, do not touch the swapchain in this state.
  if (!codec.IsReady()) return;
  if (!codec.IsPassCompiled("ApplyGrade")) {
    if (codec.IsPassFailed("ApplyGrade") && !grade_backbuffer_failure_logged_) {
      grade_backbuffer_failure_logged_ = true;
      reshade::log::message(
          reshade::log::level::error,
          "DLSSNR: ApplyGrade shader failed to compile; backbuffer grade disabled");
    }
    return;
  }

  ID3D12GraphicsCommandList* list = BeginReleaseCommands(command_queue);
  if (list == nullptr) return;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
      swapchain3 == nullptr) {
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
  const UINT idx = swapchain3->GetCurrentBackBufferIndex();
  if (FAILED(swapchain3->GetBuffer(idx, IID_PPV_ARGS(&backbuffer))) ||
      backbuffer == nullptr) {
    return;
  }

  const D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
  const unsigned int width = static_cast<unsigned int>(desc.Width);
  const unsigned int height = desc.Height;

  // Resolved before the slot check: "is this slot still usable" includes
  // "is it on this device".
  Microsoft::WRL::ComPtr<ID3D12Device> device =
      sl_compat::NativeDeviceOf(command_queue);
  if (device == nullptr) return;

  // The previous frame may still be reading these scratch textures: the
  // release list no longer waits every Present, so replacement retires them
  // through the shared queue.
  if (!grade_scratch_.Matches(device.Get(), width, height, desc.Format) ||
      !grade_scratch_out_.Matches(device.Get(), width, height, desc.Format)) {
    if (!grade_scratch_.EnsureUav(device.Get(), desc, "grade_scratch")) {
      return;
    }
    if (!grade_scratch_out_.EnsureUav(device.Get(), desc,
                                             "grade_scratch_out")) {
      return;
    }
  }

  // The backbuffer goes in as a copy source and comes back as a copy
  // destination, and never as a UAV. ApplyGrade grades `target` in place, so
  // it is handed the two textures we own: the source is parked in
  // grade_scratch_out_ first, and the result is copied back afterwards.
  release_command_.SetResourceState(backbuffer.Get(),
                                    D3D12_RESOURCE_STATE_PRESENT);
  release_command_.Transition(list, backbuffer.Get(),
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
  release_command_.Transition(list, grade_scratch_out_.Get(),
                              D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(grade_scratch_out_.Get(), backbuffer.Get());

  const bool graded = model_pipeline::ApplyGrade(
      release_command_, list, &codec, grade_scratch_out_.Get(),
      grade_scratch_out_.Get(), grade_scratch_.Get(), width, height,
      HdrFormatCode(DetectFormatFromDxgi(desc.Format)), grade);

  if (!graded) {
    TRACE_EVT(this, "grade_backbuffer_failed");
    if (!grade_backbuffer_failure_logged_) {
      grade_backbuffer_failure_logged_ = true;
      reshade::log::message(
          reshade::log::level::error,
          "DLSSNR: backbuffer grade failed after ApplyGrade was ready; "
          "returning the swapchain image to PRESENT");
    }
  } else {
    release_command_.Transition(list, grade_scratch_out_.Get(),
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    release_command_.Transition(list, backbuffer.Get(),
                                D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer.Get(), grade_scratch_out_.Get());
  }

  // Always, including on failure. Returning early would leave the swapchain's
  // image in a state the game's next Present rejects -- which removes the
  // device with DXGI_ERROR_INVALID_CALL and hides the grade failure that
  // actually happened.
  release_command_.Transition(list, backbuffer.Get(),
                              D3D12_RESOURCE_STATE_PRESENT);
}


bool AddonController::HasUnfencedRetiredSessions() const {
  for (const RetiredPassSession& retired : retired_sessions_) {
    if (retired.session != nullptr && retired.fence_value == 0) return true;
  }
  return false;
}

void AddonController::TagRetiredSessions(uint64_t fence_value) {
  if (fence_value == 0) return;
  for (RetiredPassSession& retired : retired_sessions_) {
    if (retired.session != nullptr && retired.fence_value == 0) {
      retired.fence_value = fence_value;
    }
  }
}

void AddonController::ReleaseRetiredSessionsCompleted(
    uint64_t completed_fence_value) {
  for (auto it = retired_sessions_.begin(); it != retired_sessions_.end();) {
    if (it->session != nullptr && it->fence_value != 0 &&
        it->fence_value <= completed_fence_value) {
      // Only now can ReleaseFeature and PassResources::Shutdown safely drop
      // objects that an earlier NGX/GPU command may still reference.
      it->session->Shutdown(runtime_);
      it = retired_sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

bool AddonController::DrainDevice() {
  DLSSNR_PROFILE_ASYNC_L2("AddonController::DrainDevice");
  ID3D12CommandQueue* const queue = last_command_queue_.Get();
  if (queue == nullptr) return false;

  Microsoft::WRL::ComPtr<ID3D12Device> device =
      sl_compat::NativeDeviceOf(queue);
  if (device == nullptr) return false;

  // A fence of its own, not the release context's. This marker is the last
  // thing the old device does, and a wait that gives up must not leave the
  // context a signal that its own shutdown would then block on.
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence))) ||
      fence == nullptr) {
    return false;
  }
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (event == nullptr) return false;

  // Nothing has to be recorded. The marker only has to land behind work that
  // is already queued, and a queue signal does that on its own.
  const auto start = std::chrono::steady_clock::now();
  bool idle = false;
  if (SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
      SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
    idle =
        WaitForSingleObject(event, kDeviceSwapDrainTimeoutMs) == WAIT_OBJECT_0;
  }
  CloseHandle(event);

  const long long elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count();

  TRACE_EVT(this, "device_drain idle=%d elapsed_ms=%lld", idle ? 1 : 0,
            elapsed_ms);
  if (elapsed_ms >= kDeviceSwapDrainLogMs) {
    char message[160];
    std::snprintf(
        message, sizeof(message),
        "DLSSNR: waited %lld ms for the previous device to go idle before the "
        "swap",
        elapsed_ms);
    reshade::log::message(reshade::log::level::info, message);
  }
  return idle;
}

void AddonController::EnsureDevice(ID3D12Device* device) {
  DLSSNR_PROFILE_CORE_L3("AddonController::EnsureDevice");
  if (device == nullptr || device == device_.Get()) return;

  if (device_ != nullptr) {
    // Everything the addon owns belongs to the old device. Prove that device
    // idle before releasing anything a queued command may still read. A removed
    // device has already abandoned its queue, so there is nothing to wait for.
    const HRESULT removed = device_->GetDeviceRemovedReason();
    if (removed != S_OK) {
      reshade::log::message(
          reshade::log::level::warning,
          "DLSSNR: the D3D12 device was removed; releasing the runtime without "
          "waiting for it");
    } else if (!DrainDevice()) {
      reshade::log::message(
          reshade::log::level::warning,
          "DLSSNR: the previous D3D12 device did not go idle before the swap; "
          "releasing its resources anyway");
    }

    TeardownRuntime();
    // A pending restart's fence belonged to the torn-down device, and the
    // teardown above did the work it wanted.
    restart_requested_ = false;
    restart_waiting_for_fence_ = false;
    restart_fence_value_ = 0;
    reshade::log::message(
        reshade::log::level::info,
        "DLSSNR: D3D12 device changed; reinitializing on the new device");
  }

  device_ = device;
}

void AddonController::EnsureDeviceForCommandList(
    ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  EnsureDevice(sl_compat::NativeDeviceOf(command_list).Get());
}

void AddonController::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  DLSSNR_PROFILE_CORE_L1("AddonController::OnPresent");
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (++frame_id_ == 0) frame_id_ = 1;

  // Before last_command_queue_ is overwritten: a swap drains through the queue
  // that belongs to the device being replaced.
  EnsureDevice(sl_compat::NativeDeviceOf(command_queue).Get());

  if (command_queue != nullptr && last_command_queue_.Get() != command_queue) {
    last_command_queue_ = command_queue;
  }

  // Every path below ends in the same place: SyncRetiredResources tags whatever
  // was retired this frame against a queue marker and frees the completed
  // prefix. Paths only decide whether they produced a marker.
  uint64_t marker = 0;
  bool sync = true;

  if (restart_waiting_for_fence_) {
    // Every entry retired before the restart already carries the restart
    // marker, so NeedsQueueMarker() is false and no further marker is
    // submitted while we wait.
    const uint64_t completed_fence = release_command_.CompletedFenceValue();
    if (restart_fence_value_ == 0 || completed_fence < restart_fence_value_) {
      frame_debug_.Reset();
      pre_stage_seen_ = false;
    } else {
      FinishRestartAfterFence();
      // Everything was released unconditionally; there is nothing left to sync.
      sync = false;
    }
  } else if (restart_requested_) {
    marker = SignalReleaseMarker(command_queue);
    if (marker == 0) {
      // No queue at all is not an error: the restart simply waits for one.
      if (command_queue != nullptr) {
        reshade::log::message(
            reshade::log::level::error,
            "DLSSNR: restart deferred because queue fence could not be signaled");
      }
    } else {
      restart_requested_ = false;
      restart_waiting_for_fence_ = true;
      restart_fence_value_ = marker;
      TRACE_EVT(this, "restart_deferred fence=%llu",
                static_cast<unsigned long long>(marker));
    }
    frame_debug_.Reset();
    pre_stage_seen_ = false;
  } else {
    marker = RunPresentFrame(command_queue, swapchain);
  }

  if (sync) SyncRetiredResources(command_queue, marker);
}

uint64_t AddonController::RunPresentFrame(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  const Settings& settings = GetSettings();

  if (!settings.enabled) {
    if (command_queue != nullptr) SignalHdrAtPresent(command_queue);
    AdvanceMaskFrames(false);
    // The effect can be disabled after a mode/resource change retired objects.
    // Only a marker is owed; do not run grading/debug steps while the master
    // switch is off. Returning 0 lets the caller submit that marker.
    frame_debug_.Reset();
    pre_stage_seen_ = false;
    return 0;
  }
  if (command_queue == nullptr || swapchain == nullptr) {
    if (command_queue != nullptr) SignalHdrAtPresent(command_queue);
    AdvanceMaskFrames(true);
    frame_debug_.Reset();
    pre_stage_seen_ = false;
    return 0;
  }

  // Start a new frame for the runner's skip diagnostics, so a skip that has
  // cleared is forgotten and a recurring one logs again. Present is the one
  // entry point that runs every frame, so the counter belongs here.
  PipelineRunner::BeginSkipLogFrame();
  TRACE_BEGIN_FRAME(FrameStream::Present);

  TRACE_EVT(this, "controller_enter stage=Present fmt_before=%d",
            static_cast<int>(backbuffer_format_));

  // Refresh the cached backbuffer format. Used by the debug output buffer
  // so it is always created in a format the backbuffer copy can accept.
  RefreshBackbufferFormat(swapchain);
  frame_debug_.preferred_format = backbuffer_format_;

  const PipelineStageDef* stage =
      FindPipelineStageForInjection(static_cast<int>(InjectionStage::Backbuffer));
  if (stage != nullptr) {
    for (const EnabledPass& pass : EnabledPasses(settings)) {
      if (pass.target != static_cast<int>(InjectionStage::Backbuffer)) continue;
      TRACE_EVT(this, "controller_dispatch stage=Present pass=%u",
                pass.id);
      RunPassStage(*stage, pass.id, nullptr, nullptr, false,
                   command_queue, swapchain, nullptr);
    }
  }

  // The Release stage submits the queue marker whenever any of its steps record
  // (grade, debug present). When none do, it submits nothing and the caller's
  // tail supplies an empty marker instead - so an idle frame still does not pay
  // for a submit.
  const uint64_t fence_before_release = release_command_.LastSignaledFenceValue();
  RunSingleStage(PipelineReleaseStage(), command_queue, swapchain);
  const uint64_t release_fence = release_command_.LastSignaledFenceValue();

  RefreshStatus(settings);
  AdvanceMaskFrames(true);
  // The debug image is not cleared here. A frame can be presented more than
  // once per evaluation (frame generation does exactly that), and clearing at
  // Present left every presentation after the first with no overlay. Each
  // evaluate entry point resets it instead, so it stays valid for every
  // presentation of the same evaluation and is replaced by the next one.
  pre_stage_seen_ = false;
  TRACE_EVT(this, "controller_exit stage=Present");
  TRACE_END_FRAME(FrameStream::Present);

  return release_fence > fence_before_release ? release_fence : 0;
}

uint64_t AddonController::SignalReleaseMarker(
    ID3D12CommandQueue* command_queue) {
  if (command_queue == nullptr) return 0;

  const uint64_t before_marker = release_command_.LastSignaledFenceValue();
  if (BeginReleaseCommands(command_queue) == nullptr) {
    // The release list could not be opened (for example because the command
    // context failed to initialize). The temporal feedback copy was still
    // recorded on the game's list with a predicted fence value. Signal that
    // value directly on the queue so the readback can complete; otherwise
    // can_skip is permanently false and the model runs every frame while the
    // temporal passes are pure overhead.
    if (temporal_feedback_pending_) {
      ID3D12Fence* feedback_fence = release_command_.GetFence();
      const uint64_t predicted = release_command_.GetNextFenceValue() - 1;
      if (feedback_fence != nullptr && predicted != 0) {
        command_queue->Signal(feedback_fence, predicted);
        temporal_feedback_pending_ = false;
        TRACE_EVT(this, "feedback_fence_direct_signal value=%llu",
                  static_cast<unsigned long long>(predicted));
      }
    }
    reshade::log::message(
        reshade::log::level::warning,
        "DLSSNR: deferred resource release; GPU queue marker unavailable");
    return 0;
  }

  EndReleaseCommands(command_queue);
  const uint64_t marker = release_command_.LastSignaledFenceValue();
  if (marker <= before_marker) {
    reshade::log::message(
        reshade::log::level::warning,
        "DLSSNR: deferred resource release / temporal feedback; GPU queue marker failed");
    return 0;
  }
  return marker;
}

bool AddonController::NeedsQueueMarker() const {
  return ResourceRetireQueue::Get().HasUntagged() ||
         HasUnfencedRetiredSessions() || temporal_feedback_pending_;
}

void AddonController::SyncRetiredResources(
    ID3D12CommandQueue* command_queue,
    uint64_t marker) {
  ResourceRetireQueue& retired = ResourceRetireQueue::Get();

  // Asked once, so the decision to signal and the decision to tag cannot
  // disagree.
  const bool needs_marker = NeedsQueueMarker();

  // Nothing signaled a marker this frame but work is waiting on one. Submit an
  // empty one rather than letting the queue grow until the next busy frame.
  if (marker == 0 && needs_marker) marker = SignalReleaseMarker(command_queue);

  if (marker != 0 && needs_marker) {
    retired.TagAll(marker);
    TagRetiredSessions(marker);
    temporal_feedback_pending_ = false;
  }

  const uint64_t completed = release_command_.CompletedFenceValue();
  retired.ReleaseCompleted(completed);
  ReleaseRetiredSessionsCompleted(completed);
}


void AddonController::OnPreDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* input_resource,
    ID3D12Resource* exposure_texture) {
  DLSSNR_PROFILE_CORE_L1("AddonController::OnPreDlssEvaluate");
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  // Ahead of the guards below: guide clones and tracked command-list state are
  // device-bound too, and the guides are captured before this call.
  EnsureDevice(sl_compat::NativeDeviceOf(command_list).Get());

  const Settings& settings = GetSettings();
  if (restart_requested_ || restart_waiting_for_fence_ || !settings.enabled ||
      command_list == nullptr || input_resource == nullptr)
    return;

  bool any_pre_dlss = false;
  for (const EnabledPass& pass : EnabledPasses(settings)) {
    if (pass.target == static_cast<int>(InjectionStage::PreDLSS)) {
      any_pre_dlss = true;
      break;
    }
  }
  if (!any_pre_dlss) return;

  TRACE_BEGIN_FRAME(FrameStream::PreDlss);
  TRACE_EVT(this, "controller_enter stage=PreDLSS hdr_init=%d",
            codec_.IsReady() ? 1 : 0);

  // One debug image per evaluate call. If PostDLSS follows, it preserves this
  // target and only overwrites it when a selected PostDLSS pass renders a view.
  frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;
  pre_stage_seen_ = true;

  const PipelineStageDef* stage = FindPipelineStageForInjection(
      static_cast<int>(InjectionStage::PreDLSS));
  if (stage != nullptr) {
    // One debug image per evaluate call. If PostDLSS follows, it preserves this
    // one, so the debug view stays stable across the whole evaluation.
    bool is_first = true;
    HdrState* previous_hdr = nullptr;
    for (const EnabledPass& pass : EnabledPasses(settings)) {
      if (pass.target != static_cast<int>(InjectionStage::PreDLSS)) continue;
      TRACE_EVT(this, "controller_dispatch stage=PreDLSS pass=%u is_first=%d",
                pass.id, is_first ? 1 : 0);
      const PipelineStatus status = RunPassStage(
          *stage, pass.id, command_list, input_resource, is_first, nullptr,
          nullptr, exposure_texture, previous_hdr);
      auto found = sessions_.find(pass.id);
      if (status == PipelineStatus::Ok && found != sessions_.end() &&
          found->second != nullptr) {
        previous_hdr = &found->second->Hdr();
      }
      is_first = false;
    }
  }

  RefreshStatus(settings);
  TRACE_EVT(this, "controller_exit stage=PreDLSS");
  TRACE_END_FRAME(FrameStream::PreDlss);
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource,
    ID3D12Resource* exposure_texture) {
  DLSSNR_PROFILE_CORE_L1("AddonController::OnPostDlssEvaluate");
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  // A game that only runs Post-DLSS passes reaches a swapped device here first.
  EnsureDevice(sl_compat::NativeDeviceOf(command_list).Get());

  const Settings& settings = GetSettings();
  if (restart_requested_ || restart_waiting_for_fence_ || !settings.enabled)
    return;
  if (command_list == nullptr || output_resource == nullptr) return;

  TRACE_BEGIN_FRAME(FrameStream::PostDlss);
  TRACE_EVT(this, "controller_enter stage=PostDLSS hdr_init=%d guides_out=%d",
            codec_.IsReady() ? 1 : 0, 0);

  // The PreDLSS entry resets debug state for this EvaluateFeature call. Keep
  // its output unless a selected PostDLSS pass replaces it; otherwise reset
  // here for the existing post-only configuration.
  if (!pre_stage_seen_) frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;

  const PipelineStageDef* stage = FindPipelineStageForInjection(
      static_cast<int>(InjectionStage::PostDLSS));
  if (stage != nullptr) {
    // `is_first` tracks the first ENABLED Post-DLSS pass, not index 0: the
    // first pass in the chain is the one that reads the captured DLSS output,
    // and it stays the first even if earlier passes are disabled or set to
    // the Release stage.
    bool is_first = true;
    HdrState* previous_hdr = nullptr;
    for (const EnabledPass& pass : EnabledPasses(settings)) {
      if (pass.target != static_cast<int>(InjectionStage::PostDLSS)) continue;
      TRACE_EVT(this, "controller_dispatch stage=PostDLSS pass=%u is_first=%d",
                pass.id, is_first ? 1 : 0);
      const PipelineStatus status = RunPassStage(
          *stage, pass.id, command_list, output_resource, is_first, nullptr,
          nullptr, exposure_texture, previous_hdr);
      auto found = sessions_.find(pass.id);
      if (status == PipelineStatus::Ok && found != sessions_.end() &&
          found->second != nullptr) {
        previous_hdr = &found->second->Hdr();
      }
      is_first = false;
    }
  }

  RefreshStatus(settings);
  TRACE_EVT(this, "controller_exit stage=PostDLSS");
  TRACE_END_FRAME(FrameStream::PostDlss);
}

void AddonController::PresentDebugOutput(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain,
    const FrameDebugContext& debug) {
  if (!debug.valid || debug.output == nullptr) return;

  Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain3;
  if (FAILED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain3))) ||
      swapchain3 == nullptr) {
    return;
  }

  Microsoft::WRL::ComPtr<ID3D12Resource> backbuffer;
  const UINT idx = swapchain3->GetCurrentBackBufferIndex();
  if (FAILED(swapchain3->GetBuffer(idx, IID_PPV_ARGS(&backbuffer))) ||
      backbuffer == nullptr) {
    return;
  }

  // CopyResource requires exact format and size matches. The debug output
  // is created at the cached backbuffer format, but sizes can differ if
  // the source pipeline stage ran at a different resolution. In that case
  // skip the override so we don't break the present.
  const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
  if (bb_desc.Format != debug.format()) {
    TRACE_EVT(this, "skip_present_debug reason=format_mismatch bb=%d dbg=%d",
              static_cast<int>(bb_desc.Format),
              static_cast<int>(debug.format()));
    return;
  }
  if (bb_desc.Width != debug.width() || bb_desc.Height != debug.height()) {
    TRACE_EVT(this, "skip_present_debug reason=size_mismatch bb=%llux%llu dbg=%ux%u",
              static_cast<unsigned long long>(bb_desc.Width),
              static_cast<unsigned long long>(bb_desc.Height), debug.width(),
              debug.height());
    return;
  }

  // Records onto the Release stage's list rather than submitting here, so
  // anything else the stage records rides in the same submit. The checks
  // above come first on purpose: a debug image that cannot be copied must
  // not open a list.
  ID3D12GraphicsCommandList* list = BeginReleaseCommands(command_queue);
  if (list == nullptr) return;

  release_command_.SetResourceState(backbuffer.Get(),
                                    D3D12_RESOURCE_STATE_PRESENT);
  release_command_.SetResourceState(debug.output.Get(),
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  // The debug image was written by a compute dispatch on the pass's list, not
  // this one. Same queue, so the copy runs after the dispatch, but the write
  // is only guaranteed visible to the copy after a barrier.
  release_command_.UavBarrier(list, debug.output.Get());
  release_command_.CopyResourceToBackbuffer(list, debug.output.Get(),
                                            backbuffer.Get());
}

HdrSnapshot AddonController::GetHdrSnapshot() const {
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  const auto active = sessions_.find(GetSettings().active_pass_id);
  if (active != sessions_.end() && active->second != nullptr)
    return active->second->Hdr().GetSnapshot();
  for (const auto& entry : sessions_) {
    if (entry.second != nullptr && entry.second->Hdr().Enabled())
      return entry.second->Hdr().GetSnapshot();
  }
  return {};
}

StatusSnapshot AddonController::GetStatus() const {
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  return status_;
}

void AddonController::SetPreDlssBlocked(const char* reason) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (reason == nullptr || reason[0] == '\0') {
    pre_dlss_blocked_ = false;
    pre_dlss_block_reason_[0] = '\0';
    return;
  }

  pre_dlss_blocked_ = true;
  std::snprintf(pre_dlss_block_reason_, sizeof(pre_dlss_block_reason_), "%s",
                reason);
}

void AddonController::SetPostDlssBlocked(const char* reason) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (reason == nullptr || reason[0] == '\0') {
    post_dlss_blocked_ = false;
    post_dlss_block_reason_[0] = '\0';
    return;
  }

  post_dlss_blocked_ = true;
  std::snprintf(post_dlss_block_reason_, sizeof(post_dlss_block_reason_),
                "%s", reason);
}

void AddonController::RestartAll() {
  DLSSNR_PROFILE_CORE_L1("AddonController::RestartAll");
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  restart_requested_ = true;
  reshade::log::message(reshade::log::level::info,
                        "DLSSNR: addon restart requested; waiting for a safe GPU fence");
}

bool AddonController::RestartPending() const {
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  return restart_requested_ || restart_waiting_for_fence_;
}

void AddonController::TeardownRuntime() {
  DLSSNR_PROFILE_ASYNC_L2("AddonController::TeardownRuntime");

  // Sessions hold NGX features and GPU resources that command lists submitted
  // earlier may still reference. This only runs once the queue is provably
  // idle, so they can be released here without a further fence.
  for (auto& entry : sessions_) {
    if (entry.second) entry.second->Shutdown(runtime_);
  }
  sessions_.clear();
  for (auto& retired : retired_sessions_) {
    if (retired.session) retired.session->Shutdown(runtime_);
  }
  retired_sessions_.clear();

  // Guides retire their own references into the queue rather than freeing them,
  // so the force-release at the bottom of this function covers them too.
  DlssGuides::Get().Clear();

  // Tracked game objects name the device that is going away, and the references
  // would keep it alive.
  D3D12StateTracker::Get().Clear();
  profiling::ShutdownGpu();

  release_command_.Shutdown();
  last_command_queue_.Reset();
  release_list_begun_ = false;
  temporal_feedback_pending_ = false;
  grade_scratch_.Reset();
  grade_scratch_out_.Reset();
  pass1_command_.Shutdown();
  codec_.Destroy();
  frame_debug_.Release();
  pre_stage_seen_ = false;
  runtime_.Shutdown();

  // Nothing will signal another fence, so drop everything retired on the way
  // down - including the debug image released just above. The GPU is idle here
  // by the caller's contract.
  ResourceRetireQueue::Get().ReleaseAllAfterGpuIdle();
  device_.Reset();
}

void AddonController::FinishRestartAfterFence() {
  DLSSNR_PROFILE_ASYNC_L2("AddonController::FinishRestartAfterFence");
  // The restart marker has completed, so every NGX feature and addon resource
  // can be released without racing the game's submitted command lists.
  TeardownRuntime();

  post_dlss_blocked_ = false;
  post_dlss_block_reason_[0] = '\0';
  pre_dlss_blocked_ = false;
  pre_dlss_block_reason_[0] = '\0';
  status_.Reset();
  restart_waiting_for_fence_ = false;
  restart_fence_value_ = 0;
  reshade::log::message(reshade::log::level::info,
                        "DLSSNR: safe addon restart completed; runtime will initialize on demand");
}


void AddonController::AdvanceMaskFrames(bool master_enabled) {
  const ctl::ValueStore& store = feature::SettingsStore();
  for (auto& entry : sessions_) {
    if (entry.second == nullptr) continue;
    const ctl::InstanceKey key = feature::pass::PassKey(entry.first);
    const bool enabled = master_enabled &&
        feature::hdr::Read(store, key).enable &&
        feature::exposure_mask::Read(store, key).enable;
    entry.second->AdvanceMaskFrame(enabled);
  }
}

uint64_t AddonController::ReleaseFenceHint() const {
  // LastSignaledFenceValue is a plain load; the member is only written
  // by SignalGpu under g_direct_mutex, and the NGX hook already holds
  // that mutex when it calls CaptureGuides. Reading without a lock would
  // be safe for the load itself but is not needed here.
  return release_command_.LastSignaledFenceValue();
}

void AddonController::SignalHdrAtPresent(ID3D12CommandQueue* queue) {
  DLSSNR_PROFILE_ASYNC_L2("AddonController::SignalHdrAtPresent");
  for (auto& entry : sessions_) {
    if (entry.second != nullptr) entry.second->Hdr().SignalAtPresent(queue);
  }
  for (RetiredPassSession& entry : retired_sessions_) {
    if (entry.session != nullptr) entry.session->Hdr().SignalAtPresent(queue);
  }
}

void AddonController::RefreshStatus(const Settings& settings) {
  DLSSNR_PROFILE_CORE_L2("AddonController::RefreshStatus");
  status_.Reset();

  status_.enabled = settings.enabled;
  status_.runtime_initialized = runtime_.IsInitialized();
  if (!status_.runtime_initialized) {
    const char* err = runtime_.GetLastError();
    if (err != nullptr && err[0] != '\0') {
      std::snprintf(status_.runtime_reason, sizeof(status_.runtime_reason),
                    "%s", err);
    }
  }

  const DlssGuideState guides = DlssGuides::Get().GetState();
  status_.guides_valid = guides.valid;

  status_.post_dlss_blocked = post_dlss_blocked_;
  if (post_dlss_blocked_) {
    std::snprintf(status_.post_dlss_block_reason,
                  sizeof(status_.post_dlss_block_reason), "%s",
                  post_dlss_block_reason_);
  }

  status_.pre_dlss_blocked = pre_dlss_blocked_;
  if (pre_dlss_blocked_) {
    std::snprintf(status_.pre_dlss_block_reason,
                  sizeof(status_.pre_dlss_block_reason), "%s",
                  pre_dlss_block_reason_);
  }

  HdrSnapshot hdr = {};
  const auto active_hdr = sessions_.find(settings.active_pass_id);
  if (active_hdr != sessions_.end() && active_hdr->second != nullptr) {
    hdr = active_hdr->second->Hdr().GetSnapshot();
  } else {
    for (const auto& entry : sessions_) {
      if (entry.second != nullptr && entry.second->Hdr().Enabled()) {
        hdr = entry.second->Hdr().GetSnapshot();
        break;
      }
    }
  }
  status_.hdr_ready = hdr.ready;
  status_.hdr_exposure = hdr.exposure;
  status_.hdr_scene_white = hdr.scene_white;
  status_.gain_map_ready = hdr.gain_map_ready;
  status_.gain_map_min_log2 = hdr.gain_min_log2;
  status_.gain_map_max_log2 = hdr.gain_max_log2;
  status_.gain_map_sdr_excursion_pixels = hdr.gain_sdr_excursion_pixels;
  status_.gain_map_out_of_range_pixels = hdr.gain_out_of_range_pixels;

  ctl::ValueStore& store = feature::SettingsStore();
  status_.passes.reserve(settings.pass_order.size());
  for (feature::pass::PassId id : settings.pass_order) {
    PassStatusEntry entry;
    entry.id = id;
    auto it = sessions_.find(id);
    if (it != sessions_.end() && it->second) {
      entry.status = it->second->GetStatus();
    }
    // Enabled remains setting-owned. A not-yet-created session deliberately
    // contributes a default PassStatus with all flags false and no reason.
    entry.status.enabled =
        store.Read(feature::pass::kSection, feature::pass::PassKey(id),
                   feature::pass::kPassEnabled).as_bool;
    status_.total_vram_bytes += entry.status.resources.total_bytes;
    status_.passes.push_back(entry);
  }

  // Retired count comes from the shared queue, not from DlssGuides.
  status_.retired_resources =
      static_cast<uint32_t>(ResourceRetireQueue::Get().Count());
}

void AddonController::EnsureSessions(
    const feature::pass::PassOrder& order) {
  DLSSNR_PROFILE_RESOURCE_L2("AddonController::EnsureSessions");
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    const bool keep =
        std::find(order.begin(), order.end(), it->first) != order.end();
    if (!keep) {
      if (it->second) {
        // RunStage may have recorded Pre/Post-DLSS commands on the game's list,
        // which is submitted asynchronously by the host. Do not release its
        // NGX feature or resources here; OnPresent will fence and retire it.
        retired_sessions_.push_back({std::move(it->second), 0});
      }
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  for (feature::pass::PassId id : order) {
    if (sessions_.find(id) == sessions_.end()) {
      sessions_[id] = std::make_unique<PassSession>(&DlssGuides::Get());
    }
  }
}


void AddonController::Shutdown() {
  DLSSNR_PROFILE_CORE_L1("AddonController::Shutdown");
  grade_backbuffer_failure_logged_ = false;
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  // A swap that arrived after the last Present still has to be resolved here:
  // the release context would otherwise belong to the old device, and the final
  // marker below would be signaled on a fence from a different device.
  EnsureDevice(sl_compat::NativeDeviceOf(last_command_queue_.Get()).Get());

  // Re-read: a swap tore the old queue down, so the earlier value would be
  // dangling.
  ID3D12CommandQueue* const queue = last_command_queue_.Get();

  // Shutdown is allowed to wait. Enqueue a final marker behind submitted game
  // and addon work, then block on it: that wait is what makes the unconditional
  // release at the end of TeardownRuntime legal.
  if (queue != nullptr && BeginReleaseCommands(queue) != nullptr) {
    EndReleaseCommands(queue);
    release_command_.WaitGpu(queue);
  }

  TeardownRuntime();

  restart_requested_ = false;
  restart_waiting_for_fence_ = false;
  restart_fence_value_ = 0;
}

}  // namespace reshade_dlssnr

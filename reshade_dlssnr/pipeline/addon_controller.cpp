#include "addon_controller.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "ctl/usage/ui_store.h"
#include "ctl/values.h"
#include "dlss_guides.h"
#include "feature/debug_log/view.h"
#include "feature/grade/view.h"
#include "feature/hdr/view.h"
#include "feature/importance_mask/view.h"
#include "feature/pass/settings.h"
#include "feature/pass/view.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"
#include "state/settings.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;


// True when the mask should be allowed to build this frame: both layers have
// to be on, because the mask feeds the HDR local-exposure path. Reads the
// store -- the source of truth -- rather than a copy in Settings, so this
// stays correct once Settings stops carrying the values.
bool ImportanceMaskActive() {
  const ctl::ValueStore& store = ctl::Store();
  const ctl::InstanceKey key = ctl::InstanceKey::FromInt(0);
  return feature::importance_mask::Read(store, key).enable &&
         feature::hdr::Read(store, key).enable;
}

// Creates a UAV-capable texture matching `desc`.
//
// The grade copies its source into this and then dispatches back over the
// source, so the two descriptors have to match exactly — which is why this
// takes the source's own descriptor rather than building one from the frame
// geometry.
bool CreateUavTexture(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                      ID3D12Resource** out_resource) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;

  D3D12_RESOURCE_DESC resource_desc = desc;
  resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  return SUCCEEDED(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &resource_desc,
      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
      IID_PPV_ARGS(out_resource)));
}
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
  trace::NameResource(bb.Get(), "backbuffer");
}

PipelineStatus AddonController::RunPassStage(const PipelineStageDef& stage,
                                             feature::pass::PassId pass_id,
                                             ID3D12GraphicsCommandList* command_list,
                                             ID3D12Resource* output_resource,
                                             bool is_first_pass,
                                             ID3D12CommandQueue* command_queue,
                                             IDXGISwapChain* swapchain,
                                             ID3D12Resource* exposure_texture) {
  const Settings& settings = GetSettings();

  std::unique_ptr<PassSession>& slot = sessions_[pass_id];
  if (slot == nullptr) {
    slot = std::make_unique<PassSession>(&DlssGuides::Get());
  }

  return slot->RunStage(
      stage, settings, codec_, runtime_, command_list, output_resource,
      is_first_pass, frame_debug_, &hdr_, &pass1_command_, &importance_mask_,
      exposure_texture, command_queue, swapchain, pass_id);
}

PipelineStatus AddonController::RunSingleStage(
    const PipelineStageDef& stage,
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  const Settings& settings = GetSettings();

  PassRun run;
  run.codec = &codec_;
  run.runtime = &runtime_;
  run.hdr = &hdr_;
  run.frame_debug = &frame_debug_;
  run.settings = &settings;
  run.command_queue = command_queue;
  run.swapchain = swapchain;

  // The Release stage does not go through a PassSession, so the single-instance
  // snapshots it might read are filled here. Its steps read the backbuffer
  // grade directly (RunGradeBackbuffer), but hdr_state / mask_state are cheap
  // and keep the run self-consistent for any step later added to this stage.
  {
    const ctl::ValueStore& store = ctl::Store();
    const ctl::InstanceKey shared = ctl::InstanceKey::FromInt(0);
    run.debug_view = feature::debug_log::Read(store, shared);
    run.hdr_state = feature::hdr::Read(store, shared);
    run.mask_state = feature::importance_mask::Read(store, shared);
  }

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

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
    return nullptr;
  }
  if (!release_command_.Ensure(device.Get())) return nullptr;

  // Reset and clear once per frame, so the state a step seeds is the state
  // the list starts from. A later caller in the same frame must not do this
  // again — it would discard the commands already recorded.
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

  if (grade_scratch_ == nullptr || grade_scratch_width_ != width ||
      grade_scratch_height_ != height || grade_scratch_format_ != desc.Format) {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) ||
        device == nullptr) {
      return;
    }
    // The previous frame may still be reading these scratch textures: the
    // release list no longer waits every Present. Transfer references to the
    // same fence-delayed retire queue used for replaced pass resources.
    if (grade_scratch_ != nullptr) {
      grade_scratch_->AddRef();
      DlssGuides::Get().RetireGameResource(grade_scratch_.Get());
      grade_scratch_.Reset();
    }
    if (grade_scratch_out_ != nullptr) {
      grade_scratch_out_->AddRef();
      DlssGuides::Get().RetireGameResource(grade_scratch_out_.Get());
      grade_scratch_out_.Reset();
    }
    ID3D12Resource* raw = nullptr;
    if (!CreateUavTexture(device.Get(), desc, &raw)) return;
    grade_scratch_.Attach(raw);
    raw = nullptr;
    if (!CreateUavTexture(device.Get(), desc, &raw)) {
      grade_scratch_.Reset();
      return;
    }
    grade_scratch_out_.Attach(raw);
    grade_scratch_width_ = width;
    grade_scratch_height_ = height;
    grade_scratch_format_ = desc.Format;
    TRACE_CMD(this, "create grade_scratch res=%d %ux%u",
              trace::ResourceId(grade_scratch_.Get()), width, height);
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
      grade_scratch_.Get(), width, height, HdrFormatCode(DetectFormatFromDxgi(desc.Format)), grade);

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

bool AddonController::EnsureHdr(ID3D12Device* device) {
  if (hdr_initialized_) return true;
  if (device == nullptr) return false;
  if (!hdr_.Ensure(device) || !codec_.Init(device)) return false;
  hdr_initialized_ = true;
  return true;
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

void AddonController::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  if (command_queue != nullptr && last_command_queue_.Get() != command_queue) {
    last_command_queue_ = command_queue;
  }

  DlssGuides& guides = DlssGuides::Get();
  const uint64_t completed_fence = release_command_.CompletedFenceValue();
  guides.ReleaseRetiredResourcesCompleted(completed_fence);
  ReleaseRetiredSessionsCompleted(completed_fence);

  const Settings& settings = GetSettings();
  if (!settings.enabled) {
    importance_mask_.AdvanceFrame(false);
    // The effect can be disabled after a mode/resource change retired objects.
    // Submit an empty marker only; do not run grading/debug steps while the
    // master switch is off.
    const bool have_retired_resources =
        guides.RetiredGameResourceCount() != 0 ||
        guides.RetiredResourceCount() != 0 || HasUnfencedRetiredSessions();
    if (command_queue != nullptr && have_retired_resources &&
        BeginReleaseCommands(command_queue) != nullptr) {
      const uint64_t before_marker = release_command_.LastSignaledFenceValue();
      EndReleaseCommands(command_queue);
      const uint64_t marker = release_command_.LastSignaledFenceValue();
      if (marker > before_marker) {
        guides.TagRetiredResources(marker);
        TagRetiredSessions(marker);
      }
    }
    frame_debug_.Reset();
    pre_stage_seen_ = false;
    return;
  }
  if (command_queue == nullptr || swapchain == nullptr) {
    importance_mask_.AdvanceFrame(ImportanceMaskActive());
    frame_debug_.Reset();
    pre_stage_seen_ = false;
    return;
  }

  // Start a new frame for the runner's skip diagnostics, so a skip that has
  // cleared is forgotten and a recurring one logs again. Present is the one
  // entry point that runs every frame, so the counter belongs here.
  PipelineRunner::BeginSkipLogFrame();
  trace::BeginFrame(trace::FrameStream::Present);

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

  // Queue-retired game resources and replaced addon textures need a fence
  // behind the game's/effect's work, but must not stall every Present. Ensure
  // the Release list gets submitted even when no grade/debug dispatch is active.
  const bool have_retired_resources =
      guides.RetiredGameResourceCount() != 0 ||
      guides.RetiredResourceCount() != 0 || HasUnfencedRetiredSessions();
  const bool retirement_sync_ready =
      !have_retired_resources || BeginReleaseCommands(command_queue) != nullptr;
  const uint64_t fence_before_release = release_command_.LastSignaledFenceValue();

  RunSingleStage(PipelineReleaseStage(), command_queue, swapchain);

  if (have_retired_resources && retirement_sync_ready) {
    const uint64_t retirement_fence = release_command_.LastSignaledFenceValue();
    if (retirement_fence > fence_before_release) {
      guides.TagRetiredResources(retirement_fence);
      TagRetiredSessions(retirement_fence);
      const uint64_t completed = release_command_.CompletedFenceValue();
      guides.ReleaseRetiredResourcesCompleted(completed);
      ReleaseRetiredSessionsCompleted(completed);
    } else {
      reshade::log::message(
          reshade::log::level::warning,
          "DLSSNR: deferred resource release; GPU queue marker failed");
    }
  } else if (have_retired_resources) {
    reshade::log::message(
        reshade::log::level::warning,
        "DLSSNR: deferred resource release; GPU queue marker unavailable");
  }

  RefreshStatus(settings);
  importance_mask_.AdvanceFrame(ImportanceMaskActive());
  frame_debug_.Reset();
  pre_stage_seen_ = false;
  TRACE_EVT(this, "controller_exit stage=Present");
  trace::EndFrame(trace::FrameStream::Present);
}

void AddonController::OnPreDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* input_resource,
    ID3D12Resource* exposure_texture) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled || command_list == nullptr || input_resource == nullptr)
    return;

  bool any_pre_dlss = false;
  for (const EnabledPass& pass : EnabledPasses(settings)) {
    if (pass.target == static_cast<int>(InjectionStage::PreDLSS)) {
      any_pre_dlss = true;
      break;
    }
  }
  if (!any_pre_dlss) return;

  trace::BeginFrame(trace::FrameStream::PreDlss);
  TRACE_EVT(this, "controller_enter stage=PreDLSS hdr_init=%d",
            hdr_initialized_ ? 1 : 0);

  // One debug image per evaluate call. If PostDLSS follows, it preserves this
  // target and only overwrites it when a selected PostDLSS pass renders a view.
  frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;
  pre_stage_seen_ = true;

  // HDR metering is shared frame pre-work and uses only the previously
  // published Importance Mask. The current frame's mask is built after its
  // first successful model evaluation and is not published until Present.
  PrepareFrameForPass(command_list, input_resource, exposure_texture,
                      /*pre_dlss=*/true);

  const PipelineStageDef* stage = FindPipelineStageForInjection(
      static_cast<int>(InjectionStage::PreDLSS));
  if (stage != nullptr) {
    // One debug image per evaluate call. If PostDLSS follows, it preserves this
    // one, so the debug view stays stable across the whole evaluation.
    bool is_first = true;
    for (const EnabledPass& pass : EnabledPasses(settings)) {
      if (pass.target != static_cast<int>(InjectionStage::PreDLSS)) continue;
      TRACE_EVT(this, "controller_dispatch stage=PreDLSS pass=%u is_first=%d",
                pass.id, is_first ? 1 : 0);
      RunPassStage(*stage, pass.id, command_list,
                   input_resource, is_first, nullptr, nullptr,
                   exposure_texture);
      is_first = false;
    }
  }

  RefreshStatus(settings);
  TRACE_EVT(this, "controller_exit stage=PreDLSS");
  trace::EndFrame(trace::FrameStream::PreDlss);
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource,
    ID3D12Resource* exposure_texture) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  trace::BeginFrame(trace::FrameStream::PostDlss);
  TRACE_EVT(this, "controller_enter stage=PostDLSS hdr_init=%d guides_out=%d",
            hdr_initialized_ ? 1 : 0, 0);

  // The PreDLSS entry resets debug state for this EvaluateFeature call. Keep
  // its output unless a selected PostDLSS pass replaces it; otherwise reset
  // here for the existing post-only configuration.
  if (!pre_stage_seen_) frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;

  // If no PreDLSS pass ran in this NGX evaluation, PostDLSS is the first
  // usable target. Prepare metering from its current upscaled output before
  // dispatching the first PostDLSS pass. The current Importance Mask is not
  // published until Present.
  if (!pre_stage_seen_) {
    PrepareFrameForPass(command_list, output_resource, exposure_texture,
                        /*pre_dlss=*/false);
  }

  const PipelineStageDef* stage = FindPipelineStageForInjection(
      static_cast<int>(InjectionStage::PostDLSS));
  if (stage != nullptr) {
    // `is_first` tracks the first ENABLED Post-DLSS pass, not index 0: the
    // first pass in the chain is the one that reads the captured DLSS output,
    // and it stays the first even if earlier passes are disabled or set to
    // the Release stage.
    bool is_first = true;
    for (const EnabledPass& pass : EnabledPasses(settings)) {
      if (pass.target != static_cast<int>(InjectionStage::PostDLSS)) continue;
      TRACE_EVT(this, "controller_dispatch stage=PostDLSS pass=%u is_first=%d",
                pass.id, is_first ? 1 : 0);
      RunPassStage(*stage, pass.id, command_list,
                   output_resource, is_first, nullptr, nullptr,
                   exposure_texture);
      is_first = false;
    }
  }

  RefreshStatus(settings);
  TRACE_EVT(this, "controller_exit stage=PostDLSS");
  trace::EndFrame(trace::FrameStream::PostDlss);
}

void AddonController::PrepareFrameForPass(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source_resource,
    ID3D12Resource* exposure_texture,
    bool pre_dlss) {
  const Settings& settings = GetSettings();
  if (!settings.enabled || command_list == nullptr || source_resource == nullptr)
    return;

  // This utility stage contains the one HDR measurement and one Structure
  // Detection dispatch for the current frame. Its target metadata is switched
  // to match the earliest enabled stage, so HDR reads the same source color
  // that the first pass will see and detection uses that same current-frame
  // image instead of a stale DLSS output from the previous evaluation.
  PipelineStageDef stage = PipelineHdrPreparationStage();
  stage.is_pre_dlss = pre_dlss;
  stage.is_post_dlss = !pre_dlss;
  stage.reads_dlss_output = !pre_dlss;
  stage.output_state_managed_externally = !pre_dlss;

  TRACE_EVT(this, "controller_prepare stage=%s", pre_dlss ? "PreDLSS" : "PostDLSS");
  const PipelineStatus status = hdr_preparation_session_.RunStage(
      stage, settings, codec_, runtime_, command_list, source_resource,
      /*is_first_pass=*/true, frame_debug_, &hdr_, &pass1_command_,
      &importance_mask_, exposure_texture, nullptr, nullptr);
  // This helper session is not exposed as a user pass. A transient setup
  // failure must not permanently disable shared preparation on future frames.
  if (status == PipelineStatus::Fail) {
    hdr_preparation_session_.Retry(runtime_);
  }
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
  if (bb_desc.Format != debug.format) {
    TRACE_EVT(this, "skip_present_debug reason=format_mismatch bb=%d dbg=%d",
              static_cast<int>(bb_desc.Format), static_cast<int>(debug.format));
    return;
  }
  if (bb_desc.Width != debug.width || bb_desc.Height != debug.height) {
    TRACE_EVT(this, "skip_present_debug reason=size_mismatch bb=%llux%llu dbg=%ux%u",
              static_cast<unsigned long long>(bb_desc.Width),
              static_cast<unsigned long long>(bb_desc.Height), debug.width,
              debug.height);
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
  return hdr_.GetSnapshot();
}

StatusSnapshot AddonController::GetStatus() const {
  std::lock_guard<std::mutex> lock(g_direct_mutex);
  return status_;
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

void AddonController::RetryAll() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& entry : sessions_) {
    if (entry.second) entry.second->Retry(runtime_);
  }

  RefreshStatus(GetSettings());
}

void AddonController::RefreshStatus(const Settings& settings) {
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

  const HdrSnapshot hdr = hdr_.GetSnapshot();
  status_.hdr_ready = hdr.ready;
  status_.hdr_exposure = hdr.exposure;
  status_.hdr_scene_white = hdr.scene_white;
  status_.gain_map_ready = hdr.gain_map_ready;
  status_.gain_map_min_log2 = hdr.gain_min_log2;
  status_.gain_map_max_log2 = hdr.gain_max_log2;
  status_.gain_map_sdr_excursion_pixels = hdr.gain_sdr_excursion_pixels;
  status_.gain_map_out_of_range_pixels = hdr.gain_out_of_range_pixels;

  ctl::ValueStore& store = ctl::Store();
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
    status_.passes.push_back(entry);
  }
}

void AddonController::EnsureSessions(
    const feature::pass::PassOrder& order) {
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
  grade_backbuffer_failure_logged_ = false;
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  // Shutdown is allowed to wait. Enqueue a final marker behind submitted game
  // and addon work so feature handles/resources can be released deterministically
  // instead of relying on the non-blocking per-frame retirement path.
  ID3D12CommandQueue* const queue = last_command_queue_.Get();
  if (queue != nullptr && BeginReleaseCommands(queue) != nullptr) {
    EndReleaseCommands(queue);
    release_command_.WaitGpu(queue);
    const uint64_t fence = release_command_.LastSignaledFenceValue();
    const uint64_t completed = release_command_.CompletedFenceValue();
    if (fence != 0) {
      DlssGuides::Get().TagRetiredResources(fence);
      TagRetiredSessions(fence);
    }
    DlssGuides::Get().ReleaseRetiredResourcesCompleted(completed);
    ReleaseRetiredSessionsCompleted(completed);
  }

  for (auto& entry : sessions_) {
    if (entry.second) entry.second->Shutdown(runtime_);
  }
  sessions_.clear();
  for (auto& retired : retired_sessions_) {
    if (retired.session) retired.session->Shutdown(runtime_);
  }
  retired_sessions_.clear();
  hdr_preparation_session_.Shutdown(runtime_);
  release_command_.Shutdown();
  last_command_queue_.Reset();
  release_list_begun_ = false;
  grade_scratch_.Reset();
  grade_scratch_out_.Reset();
  grade_scratch_width_ = 0;
  grade_scratch_height_ = 0;
  grade_scratch_format_ = DXGI_FORMAT_UNKNOWN;
  pass1_command_.Shutdown();
  codec_.Destroy();
  importance_mask_.Shutdown();
  hdr_.Shutdown();
  hdr_initialized_ = false;
  frame_debug_.Release();
  runtime_.Shutdown();
}

// ---------------------------------------------------------------------------
// Release-stage step bodies
// ---------------------------------------------------------------------------
//
// The two release steps touch only controller state, so they live here rather
// than in the translation units that own the pass and HDR machinery. They are
// registered through the same lookup the runner uses for every other step.

namespace {

// Lazily creates the HDR histogram resources and initializes the compute
// engine for them. The shared frame-preparation stage calls this before the
// first PreDLSS or PostDLSS pass; later frames reuse the resources.
//
// This is controller state (hdr_initialized_ spans every pass and every
// frame), which is why the step lives here rather than in hdr_state.cpp.
PipelineStatus RunEnsureHdr(PassRun& run) {
  AddonController& controller = AddonController::Get();
  if (controller.HdrInitialized()) return PipelineStatus::Ok;
  if (run.list == nullptr) return PipelineStatus::SkipFrame;

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(run.list->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    return PipelineStatus::SkipFrame;
  }

  if (controller.EnsureHdr(device.Get())) {
    TRACE_EVT(run.session, "hdr_initialized");
  }
  // A failure here is not fatal: the pass runs without HDR measurement and
  // retries the ensure next frame.
  return PipelineStatus::Ok;
}

// Grades the final backbuffer, after every pass has run. The one grade that
// is not recorded through a pass session: it acts on the swapchain image
// rather than on a pass's own resources.
//
// Listed first in the Release stage on purpose, so the debug-present override
// below — which replaces the backbuffer wholesale — still wins.
PipelineStatus RunGradeBackbuffer(PassRun& run) {
  if (run.settings == nullptr || run.codec == nullptr) return PipelineStatus::Ok;
  if (!run.settings->enabled) return PipelineStatus::Ok;

  // Read the grade from the store -- the source of truth -- rather than from
  // a copy kept in Settings. The backbuffer grade is single-instance, so its
  // key is the same fixed key its section draws with.
  const GradeSettings grade = feature::grade::ReadBackbuffer(ctl::Store());
  if (!grade.enabled) return PipelineStatus::Ok;

  TRACE_EVT(run.session, "grade_backbuffer luma=%.3f chroma=%.3f", grade.luma,
            grade.chroma);
  AddonController::Get().GradeBackbuffer(*run.codec, run.command_queue,
                                         run.swapchain, grade);
  return PipelineStatus::Ok;
}

// Copies the debug image onto the backbuffer, if a pass produced one this
// frame. This is the last touch before the swapchain present, so it does not
// interfere with anything the game did.
PipelineStatus RunWriteDebugToBackbuffer(PassRun& run) {
  AddonController& controller = AddonController::Get();
  if (run.frame_debug == nullptr || !run.frame_debug->valid) {
    return PipelineStatus::Ok;
  }
  TRACE_EVT(run.session, "present_debug_output");
  controller.PresentDebugOutput(run.command_queue, run.swapchain,
                                *run.frame_debug);
  return PipelineStatus::Ok;
}

// Signals the HDR fence on the game's queue. The game has already submitted
// its command list, so this marks the point at which any pending histogram
// copy has completed.
PipelineStatus RunSignalHdrAtPresent(PassRun& run) {
  if (run.hdr == nullptr) return PipelineStatus::Ok;
  run.hdr->SignalAtPresent(run.command_queue);
  return PipelineStatus::Ok;
}

// `constexpr`, not `const`: a static_assert can only read an array whose
// initializer is a constant expression. `const` would compile fine for the
// runtime lookup in ControllerStepFor below and still fail the assertion at
// the end of this namespace with C2131.
constexpr PipelineNodeId kEnsureHdrRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kGradeBackbufferRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kWriteDebugRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kSignalHdrRequires[] = {PipelineNodeId::Count};

constexpr PipelineStepDef kControllerSteps[] = {
    // Reads run.list only to resolve the device, and skips cleanly when it is
    // null, so it requires nothing: Present and Release both pass no list, and
    // the step is listed in Release.
    {PipelineNodeId::EnsureHdr, "EnsureHdr", &RunEnsureHdr, kEnsureHdrRequires,
     /*required_inputs=*/StageInput::None, /*requires_hdr_aware=*/true},
    {PipelineNodeId::GradeBackbuffer, "GradeBackbuffer", &RunGradeBackbuffer,
     kGradeBackbufferRequires,
     /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue,
     /*requires_hdr_aware=*/false},
    {PipelineNodeId::WriteDebugToBackbuffer, "WriteDebugToBackbuffer",
     &RunWriteDebugToBackbuffer, kWriteDebugRequires,
     /*required_inputs=*/StageInput::Swapchain | StageInput::CommandQueue,
     /*requires_hdr_aware=*/false},
    {PipelineNodeId::SignalHdrAtPresent, "SignalHdrAtPresent",
     &RunSignalHdrAtPresent, kSignalHdrRequires,
     /*required_inputs=*/StageInput::CommandQueue, /*requires_hdr_aware=*/false},
};

constexpr StageInput kReleaseStageInputs =
    StageInput::Swapchain | StageInput::CommandQueue | StageInput::Stage;

}  // namespace

const PipelineStepDef* ControllerStepFor(PipelineNodeId id) {
  for (const PipelineStepDef& def : kControllerSteps) {
    if (def.id == id) return &def;
  }
  return nullptr;
}

// The controller's steps run in the Release stage, against the stage node list
// published in pipeline_graph.h. This is what turns "a step declares a
// prerequisite that the runner can never satisfy" from a per-frame error log
// into a build failure. Add a step here and point it at a prerequisite listed
// later in the release stage - or at one the stage never lists - and the build
// stops.
static_assert(PrerequisitesPrecedeSteps(kReleaseStepNodes,
                                        ArrayCount(kReleaseStepNodes),
                                        kControllerSteps,
                                        ArrayCount(kControllerSteps)),
              "a controller step in the Release stage declares a prerequisite "
              "that runs after it, or that the stage never lists");

// The Release stage is entered from RunSingleStage, which supplies the queue
// and swapchain but no command list. A controller step that reached for
// run.list without guarding it would dereference null here.
//
// A release step that needs to record GPU work does not get a list through
// run.list at all: it opens the stage's own list with BeginReleaseCommands,
// which returns null when the list cannot be created and lets the step skip.
// So the queue, not the list, is what a recording step must declare.
static_assert(
    StageInputsCoverSteps(kReleaseStepNodes, ArrayCount(kReleaseStepNodes),
                          kControllerSteps, ArrayCount(kControllerSteps),
                          kReleaseStageInputs),
    "a controller step in the Release stage needs an input the Release call "
    "site does not supply");

}  // namespace reshade_dlssnr

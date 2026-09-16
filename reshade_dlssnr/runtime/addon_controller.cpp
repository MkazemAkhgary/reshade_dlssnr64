#include "addon_controller.h"

#include <cstdio>
#include <mutex>

#include "dlss_guides.h"
#include "pipeline_graph.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;
}

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

  backbuffer_format_ = bb->GetDesc().Format;
  trace::NameResource(bb.Get(), "backbuffer");
}

PipelineStatus AddonController::RunPassStage(const PipelineStageDef& stage,
                                             uint32_t pass_index,
                                             ID3D12GraphicsCommandList* command_list,
                                             ID3D12Resource* output_resource,
                                             bool is_first_pass,
                                             ID3D12CommandQueue* command_queue,
                                             IDXGISwapChain* swapchain) {
  const Settings& settings = GetSettings();
  if (pass_index >= kMaxPasses) return PipelineStatus::SkipFrame;

  return passes_[pass_index].RunStage(
      stage, settings, codec_, runtime_, command_list, output_resource,
      is_first_pass, frame_debug_, &hdr_, &pass1_command_, command_queue,
      swapchain);
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

  return PipelineRunner::RunStage(stage, run);
}

bool AddonController::EnsureHdr(ID3D12Device* device) {
  if (hdr_initialized_) return true;
  if (device == nullptr) return false;
  if (!hdr_.Ensure(device) || !codec_.Init(device)) return false;
  hdr_initialized_ = true;
  return true;
}

void AddonController::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) { frame_debug_.Reset(); return; }
  if (command_queue == nullptr || swapchain == nullptr) {
    frame_debug_.Reset();
    return;
  }

  // Start a new frame for the runner's skip diagnostics, so a skip that has
  // cleared is forgotten and a recurring one logs again. Present is the one
  // entry point that runs every frame, so the counter belongs here.
  PipelineRunner::BeginSkipLogFrame();

  TRACE_EVT(this, "controller_enter stage=Present fmt_before=%d",
            static_cast<int>(backbuffer_format_));

  // Refresh the cached backbuffer format. Used by the debug output buffer
  // so it is always created in a format the backbuffer copy can accept.
  RefreshBackbufferFormat(swapchain);
  frame_debug_.preferred_format = backbuffer_format_;

  const PipelineStageDef* stage =
      FindPipelineStageForInjection(static_cast<int>(InjectionStage::Present));
  if (stage != nullptr) {
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      if (!settings.pass_enabled[i]) continue;
      if (settings.passes[i].injection_stage != InjectionStage::Present) continue;
      TRACE_EVT(this, "controller_dispatch stage=Present pass=%u", i);
      RunPassStage(*stage, i, nullptr, nullptr, false, command_queue, swapchain);
    }
  }

  RunSingleStage(PipelineReleaseStage(), command_queue, swapchain);

  RefreshStatus(settings);
  frame_debug_.Reset();
  TRACE_EVT(this, "controller_exit stage=Present");
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  TRACE_EVT(this, "controller_enter stage=PostDLSS hdr_init=%d guides_out=%d",
            hdr_initialized_ ? 1 : 0, 0);

  // Fresh debug state for the frame's Post-DLSS chain. The debug output
  // must still match the backbuffer's format (we cached it last Present),
  // because that's what it will eventually be copied into.
  frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;

  const PipelineStageDef* stage = FindPipelineStageForInjection(
      static_cast<int>(InjectionStage::PostDLSS));
  if (stage != nullptr) {
    // `is_first` tracks the first ENABLED Post-DLSS pass, not index 0: the
    // first pass in the chain is the one that reads the captured DLSS output,
    // and it stays the first even if earlier passes are disabled or set to
    // the Present stage.
    bool is_first = true;
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      if (!settings.pass_enabled[i]) continue;
      if (settings.passes[i].injection_stage != InjectionStage::PostDLSS) continue;
      TRACE_EVT(this, "controller_dispatch stage=PostDLSS pass=%u is_first=%d", i,
                is_first ? 1 : 0);
      RunPassStage(*stage, i, command_list, output_resource, is_first, nullptr,
                   nullptr);
      is_first = false;
    }
  }

  RefreshStatus(settings);
  TRACE_EVT(this, "controller_exit stage=PostDLSS");
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

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_queue->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
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

  if (!debug_command_.Ensure(device.Get())) return;

  debug_command_.Reset();
  debug_command_.ClearState();
  debug_command_.SetResourceState(backbuffer.Get(),
                                  D3D12_RESOURCE_STATE_PRESENT);
  debug_command_.SetResourceState(debug.output.Get(),
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12GraphicsCommandList* list = debug_command_.GetCommandList();
  debug_command_.CopyResourceToBackbuffer(list, debug.output.Get(),
                                          backbuffer.Get());

  debug_command_.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {debug_command_.GetCommandList()};
  command_queue->ExecuteCommandLists(1, lists);
  debug_command_.WaitGpu(command_queue);
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

  for (auto& pass : passes_) {
    pass.Retry(runtime_);
  }

  RefreshStatus(GetSettings());
}

void AddonController::RefreshStatus(const Settings& settings) {
  status_ = StatusSnapshot{};

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
  status_.hdr_white_point = hdr.white_point;

  status_.pass_count = kMaxPasses;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    // Take the runtime-owned part of the status from the session, then fill
    // in the setting-owned part. The injection stage is not stored here —
    // it is a setting, and the UI reads it directly from Settings.
    status_.passes[i] = passes_[i].GetStatus();
    status_.passes[i].enabled = settings.pass_enabled[i];
  }
}

void AddonController::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& pass : passes_) {
    pass.Shutdown(runtime_);
  }
  debug_command_.Shutdown();
  pass1_command_.Shutdown();
  codec_.Destroy();
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
// engine for them. Runs once, on the first Post-DLSS frame; after that both
// are reused and BeginFrame does the per-frame work.
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
constexpr PipelineNodeId kWriteDebugRequires[] = {PipelineNodeId::Count};
constexpr PipelineNodeId kSignalHdrRequires[] = {PipelineNodeId::Count};

constexpr PipelineStepDef kControllerSteps[] = {
    {PipelineNodeId::EnsureHdr, "EnsureHdr", &RunEnsureHdr, kEnsureHdrRequires,
     /*requires_hdr_aware=*/true},
    {PipelineNodeId::WriteDebugToBackbuffer, "WriteDebugToBackbuffer",
     &RunWriteDebugToBackbuffer, kWriteDebugRequires,
     /*requires_hdr_aware=*/false},
    {PipelineNodeId::SignalHdrAtPresent, "SignalHdrAtPresent",
     &RunSignalHdrAtPresent, kSignalHdrRequires, /*requires_hdr_aware=*/false},
};

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

}  // namespace reshade_dlssnr

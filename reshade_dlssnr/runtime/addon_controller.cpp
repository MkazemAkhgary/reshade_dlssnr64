#include "addon_controller.h"

#include <mutex>

#include "dlss_guides.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;
}

AddonController& AddonController::Get() {
  static AddonController direct;
  return direct;
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

  // Refresh the cached backbuffer format. Used by the debug output buffer
  // so it is always created in a format the backbuffer copy can accept.
  {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> sc3;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&sc3))) &&
        sc3 != nullptr) {
      Microsoft::WRL::ComPtr<ID3D12Resource> bb;
      const UINT idx = sc3->GetCurrentBackBufferIndex();
      if (SUCCEEDED(sc3->GetBuffer(idx, IID_PPV_ARGS(&bb))) && bb != nullptr) {
        backbuffer_format_ = bb->GetDesc().Format;
      }
    }
  }
  frame_debug_.preferred_format = backbuffer_format_;

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::Present) continue;
    // Resolve inherited values from earlier passes before running.
    const PassSettings resolved = ResolvePassSettings(i);
    passes_[i].OnPresent(
        runtime_,
        resolved,
        settings,
        command_queue,
        swapchain,
        frame_debug_);
  }

  // If a debug view was rendered (either by a Present pass now, or by a
  // Post-DLSS pass earlier this frame), overwrite the backbuffer with it.
  // This is the last touch before the swapchain present, so it does not
  // interfere with anything the game did.
  if (frame_debug_.valid) {
    PresentDebugOutput(command_queue, swapchain, frame_debug_);
  }

  // Signal the HDR fence on the game's queue. The game has already
  // submitted its command list, so this marks the point at which any
  // pending histogram copy has completed.
  hdr_.SignalAtPresent(command_queue);

  frame_debug_.Reset();
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  // Fresh debug state for the frame's Post-DLSS chain. The debug output
  // must still match the backbuffer's format (we cached it last Present),
  // because that's what it will eventually be copied into.
  frame_debug_.Reset();
  frame_debug_.preferred_format = backbuffer_format_;

  // HDR measurement: only the first pass in the chain runs it. It uses
  // the captured DLSS output clone as the source, since that is what the
  // model will actually consume.
  //
  // Ensure/Init run exactly once. BeginFrame itself does the per-frame
  // work: it polls for completed histogram readbacks, updates the
  // exposure EMA, and schedules new measurements at the configured rate.
  if (!hdr_initialized_) {
    Microsoft::WRL::ComPtr<ID3D12Device> hdr_device;
    if (SUCCEEDED(command_list->GetDevice(IID_PPV_ARGS(&hdr_device))) &&
        hdr_device != nullptr) {
      if (hdr_.Ensure(hdr_device.Get()) && hdr_codec_.Init(hdr_device.Get())) {
        hdr_initialized_ = true;
      }
    }
  }

  if (hdr_initialized_) {
    const DlssGuideState guides = DlssGuides::Get().GetState();
    if (guides.dlss_output_color != nullptr &&
        guides.output_width > 0 && guides.output_height > 0) {
      hdr_.BeginFrame(pass1_command_,
                      command_list,
                      guides.dlss_output_color,
                      guides.output_width,
                      guides.output_height,
                      settings,
                      hdr_codec_);
    }
  }

  bool is_first = true;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::PostDLSS) continue;
    const PassSettings resolved = ResolvePassSettings(i);
    passes_[i].OnPostDlssEvaluate(
        runtime_,
        resolved,
        settings,
        command_list,
        output_resource,
        is_first,
        frame_debug_,
        &hdr_);
    is_first = false;
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
  if (bb_desc.Format != debug.format) return;
  if (bb_desc.Width != debug.width || bb_desc.Height != debug.height) return;

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

void AddonController::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& pass : passes_) {
    pass.Shutdown(runtime_);
  }
  debug_command_.Shutdown();
  pass1_command_.Shutdown();
  hdr_codec_.Destroy();
  hdr_.Shutdown();
  hdr_initialized_ = false;
  frame_debug_.Release();
  runtime_.Shutdown();
}

}  // namespace reshade_dlssnr

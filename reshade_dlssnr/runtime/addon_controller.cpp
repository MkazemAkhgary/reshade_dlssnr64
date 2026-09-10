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

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::Present) continue;
    passes_[i].OnPresent(
        runtime_,
        settings.passes[i],
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

  frame_debug_.Reset();
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  // Fresh debug state for the frame's Post-DLSS chain.
  frame_debug_.Reset();

  bool is_first = true;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::PostDLSS) continue;
    passes_[i].OnPostDlssEvaluate(
        runtime_,
        settings.passes[i],
        settings,
        command_list,
        output_resource,
        is_first,
        frame_debug_);
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

  // The debug output is created at the pass's format. In the normal case
  // that matches the backbuffer; if it does not, skip the override so we
  // don't break the present.
  if (backbuffer->GetDesc().Format != debug.format) return;

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

void AddonController::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& pass : passes_) {
    pass.Shutdown(runtime_);
  }
  debug_command_.Shutdown();
  frame_debug_.Reset();
  frame_debug_.output.Reset();
  runtime_.Shutdown();
}

}  // namespace reshade_dlssnr

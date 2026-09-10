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

  // If a debug snapshot was captured (by a Present pass or earlier by a
  // Post-DLSS pass this frame), render it into the backbuffer now. This
  // runs at the ReShade present event, so it is the last thing that touches
  // the backbuffer before the actual swapchain present.
  if (frame_debug_.valid) {
    PresentPendingDebug(command_queue, swapchain, frame_debug_);
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

  // Note: we do NOT reset frame_debug_ here — a Present pass earlier in the
  // frame may have already captured a snapshot. We only set it if we have
  // something to capture.

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

void AddonController::PresentPendingDebug(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain,
    const FrameDebugContext& debug) {
  if (debug.debug_view <= 0) return;

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

  if (!debug_command_.Ensure(device.Get())) return;
  if (!debug_codec_.IsInit() && !debug_codec_.Init(device.Get())) return;

  const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
  const unsigned int width = static_cast<unsigned int>(bb_desc.Width);
  const unsigned int height = static_cast<unsigned int>(bb_desc.Height);

  // (Re)create the debug output buffer at backbuffer format/size.
  const bool needs_recreate =
      debug_output_ == nullptr ||
      debug_output_->GetDesc().Width != bb_desc.Width ||
      debug_output_->GetDesc().Height != bb_desc.Height ||
      debug_output_->GetDesc().Format != bb_desc.Format;

  if (needs_recreate) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = bb_desc.Width;
    desc.Height = bb_desc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = bb_desc.Format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    debug_output_.Reset();
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(debug_output_.GetAddressOf())))) {
      return;
    }
  }

  debug_command_.Reset();
  debug_command_.ClearState();
  debug_command_.SetResourceState(backbuffer.Get(),
                                  D3D12_RESOURCE_STATE_PRESENT);
  debug_command_.SetResourceState(debug_output_.Get(),
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12GraphicsCommandList* list = debug_command_.GetCommandList();

  if (!RenderDebugView(debug_command_, debug_codec_, list, debug.snapshot,
                       debug.debug_view, debug_output_.Get(), width, height)) {
    return;
  }

  debug_command_.CopyResourceToBackbuffer(list, debug_output_.Get(),
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
  debug_codec_.Destroy();
  debug_output_.Reset();
  runtime_.Shutdown();
}

}  // namespace reshade_dlssnr

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
  if (!settings.enabled) return;
  if (command_queue == nullptr || swapchain == nullptr) return;

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::Present) continue;
    passes_[i].OnPresent(
        runtime_,
        settings.passes[i],
        settings,
        command_queue,
        swapchain);
  }
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  // Run every enabled pass whose hook is Post-DLSS, in order. The first
  // such pass reads the captured DLSS output; each subsequent pass reads
  // a snapshot of the previous pass's result. The last write goes back
  // into `output_resource`.
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
        is_first);
    is_first = false;
  }
}

void AddonController::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& pass : passes_) {
    pass.Shutdown(runtime_);
  }
  runtime_.Shutdown();
}

}  // namespace reshade_dlssnr

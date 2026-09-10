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
  if (settings.injection_stage != InjectionStage::Present) return;
  if (command_queue == nullptr || swapchain == nullptr) return;

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
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
  if (settings.injection_stage != InjectionStage::PostDLSS) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  // Only the first enabled pass runs at Post-DLSS; subsequent passes are
  // ignored because they would chain into a resource we don't own.
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    passes_[i].OnPostDlssEvaluate(
        runtime_,
        settings.passes[i],
        settings,
        command_list,
        output_resource);
    break;
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

#include "dlssnr_direct.h"

#include <mutex>

#include "dlss_guides.h"

namespace reshade_dlssnr {

namespace {
std::mutex g_direct_mutex;
}

DlssNrDirect& DlssNrDirect::Get() {
  static DlssNrDirect direct;
  return direct;
}

void DlssNrDirect::OnPresent(
    ID3D12CommandQueue* command_queue,
    IDXGISwapChain* swapchain) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
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

void DlssNrDirect::Shutdown() {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  for (auto& pass : passes_) {
    pass.Shutdown(runtime_);
  }
  runtime_.Shutdown();
}

}  // namespace reshade_dlssnr

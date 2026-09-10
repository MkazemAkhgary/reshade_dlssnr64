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

  frame_debug_.Reset();

  // Find the last enabled pass whose target is the backbuffer; that pass
  // is the one that applies the debug output if one was rendered.
  int last_present_pass = -1;
  for (int i = static_cast<int>(kMaxPasses) - 1; i >= 0; --i) {
    if (settings.pass_enabled[i] &&
        settings.passes[i].injection_stage == InjectionStage::Present) {
      last_present_pass = i;
      break;
    }
  }

  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::Present) continue;
    const bool is_last = (static_cast<int>(i) == last_present_pass);
    passes_[i].OnPresent(
        runtime_,
        settings.passes[i],
        settings,
        command_queue,
        swapchain,
        frame_debug_,
        is_last);
  }
}

void AddonController::OnPostDlssEvaluate(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* output_resource) {
  std::lock_guard<std::mutex> lock(g_direct_mutex);

  const Settings& settings = GetSettings();
  if (!settings.enabled) return;
  if (command_list == nullptr || output_resource == nullptr) return;

  frame_debug_.Reset();

  // Find the last enabled Post-DLSS pass; that pass applies the debug
  // output if one was rendered.
  int last_post_dlss_pass = -1;
  for (int i = static_cast<int>(kMaxPasses) - 1; i >= 0; --i) {
    if (settings.pass_enabled[i] &&
        settings.passes[i].injection_stage == InjectionStage::PostDLSS) {
      last_post_dlss_pass = i;
      break;
    }
  }

  // Run every enabled pass whose hook is Post-DLSS, in order. The first
  // such pass reads the captured DLSS output; each subsequent pass reads
  // a snapshot of the previous pass's result. The last write goes back
  // into `output_resource`.
  bool is_first = true;
  for (uint32_t i = 0; i < kMaxPasses; ++i) {
    if (!settings.pass_enabled[i]) continue;
    if (settings.passes[i].injection_stage != InjectionStage::PostDLSS) continue;
    const bool is_last = (static_cast<int>(i) == last_post_dlss_pass);
    passes_[i].OnPostDlssEvaluate(
        runtime_,
        settings.passes[i],
        settings,
        command_list,
        output_resource,
        is_first,
        frame_debug_,
        is_last);
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

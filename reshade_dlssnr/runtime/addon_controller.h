// Controller facade owning shared runtime and per-pass sessions.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <memory>
#include <vector>

#include "dlss_guides.h"
#include "pass_session.h"
#include "runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class AddonController {
 public:
  static AddonController& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  // Runs the first enabled pass inline on the game's command list, right
  // after the DLSS pass. Only active when InjectionStage == PostDLSS.
  void OnPostDlssEvaluate(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource);

  void Shutdown();

 private:
  AddonController() {
    passes_.reserve(kMaxPasses);
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      passes_.emplace_back(&DlssGuides::Get());
    }
  }
  ~AddonController() = default;

  AddonController(const AddonController&) = delete;
  AddonController& operator=(const AddonController&) = delete;

  NgxRuntime runtime_;
  std::vector<PassSession> passes_;
};

}  // namespace reshade_dlssnr

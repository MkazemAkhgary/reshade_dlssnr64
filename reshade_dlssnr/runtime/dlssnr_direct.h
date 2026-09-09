// Controller facade owning shared runtime and per-pass sessions.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <memory>

#include "dlss_guides.h"
#include "dlssnr_pass_session.h"
#include "dlssnr_runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrDirect {
 public:
  static DlssNrDirect& Get();

  void OnPresent(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  void Shutdown();

 private:
  DlssNrDirect()
      : pass1_(&DlssGuides::Get()),
        pass2_(&DlssGuides::Get()),
        pass3_(&DlssGuides::Get()) {}
  ~DlssNrDirect() = default;

  DlssNrDirect(const DlssNrDirect&) = delete;
  DlssNrDirect& operator=(const DlssNrDirect&) = delete;

  DlssNrRuntime runtime_;
  DlssNrPassSession pass1_;
  DlssNrPassSession pass2_;
  DlssNrPassSession pass3_;
};

}  // namespace reshade_dlssnr

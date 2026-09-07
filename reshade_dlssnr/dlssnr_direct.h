#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <memory>

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

  void SetDebugSelectedResource(uint64_t handle);
  void ClearSelectedPass();
  void SetPendingSelectedPass(ID3D12Resource* resource);

  void Shutdown();

  bool IsReady() const;
  const char* GetLastError() const;

 private:
  DlssNrDirect() = default;
  ~DlssNrDirect() = default;

  DlssNrDirect(const DlssNrDirect&) = delete;
  DlssNrDirect& operator=(const DlssNrDirect&) = delete;

  DlssNrRuntime runtime_;
  DlssNrPassSession pass1_;
  DlssNrPassSession pass2_;

  bool failed_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

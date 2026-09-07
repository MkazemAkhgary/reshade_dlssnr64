#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>

#include "codec.h"
#include "dlssnr_command_context.h"
#include "dlssnr_feature_lifecycle.h"
#include "dlssnr_pass_resources.h"
#include "dlssnr_runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrDirect {
 public:
  static DlssNrDirect& Get();
  static DlssNrDirect& GetPass2();

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
  DlssNrCommandContext command_;
  DlssNrPassResources resources_;
  DlssNrFeatureLifecycle feature_;

  bool failed_ = false;
  const char* reason_ = "";
  int pass_index_ = 0;

  const PassSettings* current_pass_ = nullptr;

  ID3D12CommandQueue* command_queue_ = nullptr;

  std::unique_ptr<DlssNrCodec> debug_codec_;

  uint64_t debug_selected_resource_handle_ = 0;
  ID3D12Resource* pending_selected_pass_ = nullptr;
};

}  // namespace reshade_dlssnr

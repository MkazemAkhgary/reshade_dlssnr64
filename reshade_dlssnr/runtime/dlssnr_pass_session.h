// Per-pass orchestrator coordinating resources, feature lifecycle, and stages.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <memory>

#include "codec.h"
#include "dlssnr_command_context.h"
#include "dlssnr_feature_lifecycle.h"
#include "dlssnr_pass_resources.h"
#include "dlssnr_runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrPassSession {
 public:
  void OnPresent(
      DlssNrRuntime& runtime,
      const PassSettings& pass,
      const Settings& settings,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  void Shutdown(DlssNrRuntime& runtime);

  void SetPendingSelectedPass(ID3D12Resource* resource);

  bool IsReady() const;
  const char* GetLastError() const;

 private:
  DlssNrCommandContext command_;
  DlssNrPassResources resources_;
  DlssNrFeatureLifecycle feature_;

  ID3D12CommandQueue* command_queue_ = nullptr;
  std::unique_ptr<DlssNrCodec> debug_codec_;

  Microsoft::WRL::ComPtr<ID3D12Resource> pending_selected_pass_;

  bool failed_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

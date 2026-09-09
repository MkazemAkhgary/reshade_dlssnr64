// Per-pass orchestrator coordinating resources, feature lifecycle, and stages.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>

#include "codec.h"
#include "dlss_guides.h"
#include "dlssnr_command_context.h"
#include "dlssnr_feature_lifecycle.h"
#include "dlssnr_pass_resources.h"
#include "dlssnr_runtime.h"
#include "settings.h"

namespace reshade_dlssnr {

class DlssNrPassSession {
 public:
  explicit DlssNrPassSession(DlssGuides* guides)
      : guides_(guides) {}

  void OnPresent(
      DlssNrRuntime& runtime,
      const PassSettings& pass,
      const Settings& settings,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  void Shutdown(DlssNrRuntime& runtime);

 private:
  DlssNrCommandContext command_;
  DlssNrPassResources resources_;
  DlssNrFeatureLifecycle feature_;

  DlssGuides* guides_ = nullptr;

  ID3D12CommandQueue* command_queue_ = nullptr;
  std::unique_ptr<DlssNrCodec> debug_codec_;

  bool failed_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

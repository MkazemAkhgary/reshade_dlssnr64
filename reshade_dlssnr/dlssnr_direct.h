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

  bool EnsureFeature(ID3D12GraphicsCommandList* command_list, unsigned int width, unsigned int height);

  DlssNrRuntime runtime_;
  DlssNrCommandContext command_;
  DlssNrPassResources resources_;

  bool failed_ = false;
  bool first_evaluate_ = true;
  const char* reason_ = "";
  int pass_index_ = 0;

  NVSDK_NGX_Handle* feature_handle_ = nullptr;
  unsigned int built_width_ = 0;
  unsigned int built_height_ = 0;
  float built_working_scale_ = -1.0f;
  unsigned int built_preset_ = 0xFFFFFFFFu;
  float built_intensity_ = -1.0f;
  float built_local_structure_ = -1.0f;
  float built_local_tone_ = -1.0f;
  float built_skin_structure_ = -1.0f;
  bool built_auto_mask_ = false;
  bool built_ui_correction_ = false;

  const PassSettings* current_pass_ = nullptr;

  ID3D12CommandQueue* command_queue_ = nullptr;

  std::unique_ptr<DlssNrCodec> debug_codec_;

  uint64_t debug_selected_resource_handle_ = 0;
  ID3D12Resource* pending_selected_pass_ = nullptr;
};

}  // namespace reshade_dlssnr

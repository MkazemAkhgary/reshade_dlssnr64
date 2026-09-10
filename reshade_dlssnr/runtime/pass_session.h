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

class PassSession {
 public:
  explicit PassSession(DlssGuides* guides)
      : guides_(guides) {}

  void OnPresent(
      NgxRuntime& runtime,
      const PassSettings& pass,
      const Settings& settings,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain);

  void Shutdown(NgxRuntime& runtime);

 private:
  // Acquires the backbuffer and its descriptor for the current frame.
  bool AcquireBackbuffer(
      IDXGISwapChain* swapchain,
      ID3D12CommandQueue* command_queue,
      Microsoft::WRL::ComPtr<IDXGISwapChain3>& out_swapchain3,
      Microsoft::WRL::ComPtr<ID3D12Device>& out_device,
      Microsoft::WRL::ComPtr<ID3D12Resource>& out_backbuffer,
      D3D12_RESOURCE_DESC& out_desc);

  // Creates/resizes the per-pass output, composite, working, and scratch
  // resources required by the current frame.
  bool EnsureFrameResources(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height,
      bool use_working_scale);

  // Fills the real NGX parameter block with tuning values, resources, and
  // subrects for the current evaluation.
  bool FillEvaluateParams(
      NgxRuntime& runtime,
      const PassSettings& pass,
      const DlssGuideState& guides,
      ID3D12Resource* model_color,
      ID3D12Resource* model_output,
      unsigned int eval_width,
      unsigned int eval_height,
      unsigned int width,
      unsigned int height);

  // Closes the command list, submits it, waits for the GPU, and releases
  // retired game resources.
  void SubmitAndWait();

  CommandContext command_;
  PassResources resources_;
  FeatureLifecycle feature_;

  DlssGuides* guides_ = nullptr;

  ID3D12CommandQueue* command_queue_ = nullptr;
  std::unique_ptr<ComputeEngine> codec_;

  bool failed_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

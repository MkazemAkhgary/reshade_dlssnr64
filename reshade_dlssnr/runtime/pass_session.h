// Per-pass orchestrator coordinating resources, feature lifecycle, and stages.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>

#include "command_context.h"
#include "compute_engine.h"
#include "debug_view.h"
#include "dlss_guides.h"
#include "feature_lifecycle.h"
#include "frame_debug_context.h"
#include "hdr_state.h"
#include "model_pipeline.h"
#include "pass_resources.h"
#include "runtime.h"
#include "settings.h"
#include "status.h"

namespace reshade_dlssnr {

class PassSession {
 public:
  PassSession(DlssGuides* guides, uint32_t pass_index)
      : guides_(guides), pass_index_(pass_index) {}

  void OnPresent(
      NgxRuntime& runtime,
      const PassSettings& pass,
      const Settings& settings,
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain,
      FrameDebugContext& frame_debug);

  // Runs the model pipeline inline on the game's command list, right after
  // the DLSS pass. Writes the composed result back into `output_resource`.
  // Does not close, submit, or wait on the command list.
  //
  // `is_first_pass` is true for the first Post-DLSS pass in the chain: its
  // input is the captured DLSS output clone. Subsequent passes read a
  // snapshot of the previous pass's result from `output_resource`.
  void OnPostDlssEvaluate(
      NgxRuntime& runtime,
      const PassSettings& pass,
      const Settings& settings,
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource,
      bool is_first_pass,
      FrameDebugContext& frame_debug,
      HdrState* hdr);

  void Shutdown(NgxRuntime& runtime);

  // Snapshot of this pass's current state, for the controller's status UI.
  PassStatus GetStatus() const;

 private:
  // Sets failed_ and reason_, and logs the failure once. Use in place of
  // the `failed_ = true; reason_ = "...";` pattern so every failure has a
  // single visible log entry.
  void Fail(const char* reason);

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
      unsigned int work_height);

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

  // Registers the known frame-boundary state of every pass-owned resource.
  // Called after ClearState() so inference never has to guess for resources
  // we own. Requires that CleanupFrameState() was called at the end of the
  // previous frame.
  void SeedFrameState(ID3D12Resource* backbuffer);

  // Returns every pass-owned resource to UNORDERED_ACCESS so the next frame's
  // SeedFrameState() is truthful. Called at the very end of each pass, before
  // SubmitAndWait().
  void CleanupFrameState(ID3D12GraphicsCommandList* command_list);

  // Closes the command list, submits it, waits for the GPU, and releases
  // retired game resources.
  void SubmitAndWait();

  // If this pass is the active one (matching Settings::active_pass) and a
  // debug view is selected, renders it into `frame_debug.output`. Runs on
  // the pass's own command list, so the source resources have their true
  // states. Returns true on success. No-op otherwise.
  bool MaybeRenderDebug(
      const Settings& settings,
      ID3D12GraphicsCommandList* command_list,
      FrameDebugContext& frame_debug,
      const DebugSnapshot& snapshot,
      unsigned int width,
      unsigned int height,
      DXGI_FORMAT format);

  // Retires pass-owned resources whose size/format is about to change.
  // Wired to DlssGuides' retired-game-resource queue so the GPU is done
  // with them before they are released.
  static void RetireResourceCallback(ID3D12Resource* resource,
                                     void* user_data);

  CommandContext command_;
  PassResources resources_;
  FeatureLifecycle feature_;

  DlssGuides* guides_ = nullptr;
  uint32_t pass_index_ = 0;

  ID3D12CommandQueue* command_queue_ = nullptr;
  std::unique_ptr<ComputeEngine> codec_;

  bool failed_ = false;
  const char* reason_ = "";
};

}  // namespace reshade_dlssnr

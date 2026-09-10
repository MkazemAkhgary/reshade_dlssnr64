// Controller facade owning shared runtime and per-pass sessions.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvsdk_ngx.h>
#include <windows.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "command_context.h"
#include "dlss_guides.h"
#include "frame_debug_context.h"
#include "hdr_state.h"
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

  // Runs the enabled Post-DLSS passes inline on the game's command list,
  // right after the DLSS pass. Each pass writes its composed result back
  // into `output_resource`. The active pass (if it is a Post-DLSS pass)
  // also captures its debug view.
  void OnPostDlssEvaluate(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* output_resource);

  void Shutdown();

  // Thread-safe snapshot of the HDR measurement state, for the UI.
  HdrSnapshot GetHdrSnapshot() const;

 private:
  AddonController() {
    passes_.reserve(kMaxPasses);
    for (uint32_t i = 0; i < kMaxPasses; ++i) {
      passes_.emplace_back(&DlssGuides::Get(), i);
    }
  }
  ~AddonController() = default;

  AddonController(const AddonController&) = delete;
  AddonController& operator=(const AddonController&) = delete;

  // Copies frame_debug.output to the swapchain backbuffer. The debug image
  // was already rendered (at pass time) into a resource we own; this is
  // purely a present-time override so the game's rendering is unaffected.
  void PresentDebugOutput(
      ID3D12CommandQueue* command_queue,
      IDXGISwapChain* swapchain,
      const FrameDebugContext& debug);

  NgxRuntime runtime_;
  std::vector<PassSession> passes_;
  FrameDebugContext frame_debug_;
  HdrState hdr_;

  // Command context used only for the final debug-present copy.
  CommandContext debug_command_;

  // Command context used only for the HDR histogram dispatch (which does
  // not submit — it just records state on the game's list).
  CommandContext pass1_command_;

  // Compute engine used only for the HDR histogram passes.
  ComputeEngine hdr_codec_;

  // Backbuffer format from the last Present. The debug output is displayed
  // on the backbuffer, so we create it in this format. Cached because a
  // Post-DLSS pass (which runs before Present) needs to know the format
  // before we get a chance to query the backbuffer this frame.
  DXGI_FORMAT backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
};

}  // namespace reshade_dlssnr

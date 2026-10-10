// Per-frame debug capture. The active pass renders the selected debug view
// into `output` at pass time (so it sees the correct resource states and
// content). The Present event then just copies `output` to the backbuffer.
// This keeps the debug display entirely out of the game's rendering: no
// debug pixels ever touch the game's resources during its own rendering.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace reshade_dlssnr {

struct FrameDebugContext {
  // The debug image. Reused across frames; only recreated on size/format
  // change. This is the only resource the debug path owns.
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  unsigned int width = 0;
  unsigned int height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

  // Format the debug output should be created in. The debug image is always
  // displayed on the swapchain backbuffer, so its format should match the
  // backbuffer's, regardless of the source pipeline stage's format. Set by
  // AddonController before passes run. When UNKNOWN, falls back to the
  // pass's own source format.
  DXGI_FORMAT preferred_format = DXGI_FORMAT_UNKNOWN;
  unsigned int preferred_width = 0;
  unsigned int preferred_height = 0;

  // True when `output` holds a debug render produced this frame.
  bool valid = false;

  void Reset() {
    valid = false;
    // Note: `output`, target dimensions, `format`, and preferred backbuffer extent are intentionally kept —
    // the resource is reused across frames to avoid churn.
  }

  // Discards the cached output buffer. Call when the swapchain or device is
  // torn down.
  void Release() {
    output.Reset();
    width = 0;
    height = 0;
    format = DXGI_FORMAT_UNKNOWN;
    preferred_format = DXGI_FORMAT_UNKNOWN;
    preferred_width = 0;
    preferred_height = 0;
    valid = false;
  }
};

}  // namespace reshade_dlssnr

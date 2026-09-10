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

  // True when `output` holds a debug render produced this frame.
  bool valid = false;

  void Reset() {
    valid = false;
    // Note: `output` is intentionally not reset — it is reused next frame.
  }
};

}  // namespace reshade_dlssnr

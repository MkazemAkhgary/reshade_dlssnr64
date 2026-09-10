// Shared per-frame debug output. One resource per frame, rendered by the
// currently-selected pass and applied by the last pass in the chain.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace reshade_dlssnr {

struct FrameDebugContext {
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  unsigned int width = 0;
  unsigned int height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool valid = false;

  void Reset() {
    valid = false;
  }
};

}  // namespace reshade_dlssnr

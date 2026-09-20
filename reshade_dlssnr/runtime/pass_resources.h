// Per-pass output, UI mask, composite, working-scale, and scratch resources.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace reshade_dlssnr {

// A callback invoked when a resource is about to be replaced. The callback
// owns the transferred reference and must release it only when the GPU is
// known to be done with the resource.
using RetireCallback = void (*)(ID3D12Resource* resource, void* user_data);

// Generic scratch texture pool for intermediate stages (pre-exposure,
// blend helpers, etc.). Slots are created lazily at whatever size/format
// is requested and reused across frames until the requested size changes.
//
// Slot 0 holds the Post-DLSS multi-pass input snapshot and slot 2 the colour
// grade's in-place ping-pong; slot 1 is free.
//
// The frame debug output is deliberately not here. It lives in the
// controller's FrameDebugContext, because the Release stage copies it to the
// backbuffer after every pass has finished, so it has to outlive the pass
// session that rendered it.
constexpr uint32_t kScratchCount = 4;
constexpr uint32_t kInputSnapshotSlot = 0;
constexpr uint32_t kGradeScratchSlot = 2;

class PassResources {
 public:
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  unsigned int output_width = 0;
  unsigned int output_height = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> composite_output;
  unsigned int composite_output_width = 0;
  unsigned int composite_output_height = 0;
  DXGI_FORMAT composite_output_format = DXGI_FORMAT_UNKNOWN;

  Microsoft::WRL::ComPtr<ID3D12Resource> working_input;
  Microsoft::WRL::ComPtr<ID3D12Resource> working_output;
  unsigned int working_width = 0;
  unsigned int working_height = 0;
  DXGI_FORMAT working_format = DXGI_FORMAT_UNKNOWN;

  // Resources replaced by Ensure*() are handed to this callback rather than
  // being released immediately. The callback takes ownership of the
  // reference (which is not AddRef'd). This must always be set — a resource
  // replaced this frame may still be read by in-flight GPU work on the
  // game's queue, and immediate release causes a use-after-free (which
  // manifests as DXGI_ERROR_DEVICE_HUNG).
  void SetRetireCallback(RetireCallback cb, void* user_data) {
    retire_cb_ = cb;
    retire_user_data_ = user_data;
  }

  bool EnsureOutput(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  bool EnsureCompositeOutput(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  bool EnsureWorkingResources(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height);

  // Ensures scratch slot `index` matches the given size and format.
  bool EnsureScratch(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int width,
      unsigned int height,
      uint32_t index);

  void Shutdown();

  ID3D12Resource* GetOutput() const { return output.Get(); }
  ID3D12Resource* GetCompositeOutput() const { return composite_output.Get(); }
  ID3D12Resource* GetWorkingInput() const { return working_input.Get(); }
  ID3D12Resource* GetWorkingOutput() const { return working_output.Get(); }
  ID3D12Resource* GetScratch(uint32_t index) const {
    return index < kScratchCount ? scratch_[index].Get() : nullptr;
  }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> scratch_[kScratchCount];
  unsigned int scratch_width_[kScratchCount] = {};
  unsigned int scratch_height_[kScratchCount] = {};
  DXGI_FORMAT scratch_format_[kScratchCount] = {};

  RetireCallback retire_cb_ = nullptr;
  void* retire_user_data_ = nullptr;

  // Releases a ComPtr, either immediately or via the retire callback.
  // Call this in place of `.Reset()` for any resource that might still be
  // in use by in-flight GPU work.
  void Dispose(Microsoft::WRL::ComPtr<ID3D12Resource>& resource);

  static bool CreateUavTexture(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      ID3D12Resource** out_resource);
};

}  // namespace reshade_dlssnr

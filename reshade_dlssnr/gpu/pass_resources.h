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
// Slot 0 holds the Post-DLSS multi-pass input snapshot; slots 1 and 2 hold
// grade scratch; slots 3 and 4 hold the pre-grade model input/output debug
// snapshots.
//
// The frame debug output is deliberately not here. It lives in the
// controller's FrameDebugContext, because the Release stage copies it to the
// backbuffer after every pass has finished, so it has to outlive the pass
// session that rendered it.
constexpr uint32_t kScratchCount = 5;
constexpr uint32_t kInputSnapshotSlot = 0;
constexpr uint32_t kGradeScratchSlotA = 1;
constexpr uint32_t kGradeScratchSlotB = 2;
constexpr uint32_t kDebugPreGradeInputSlot = 3;
constexpr uint32_t kDebugPreGradeOutputSlot = 4;

class PassResources {
 public:
  // The full-resolution auxiliary target. In HDR frames with Local Exposure,
  // it holds the global-only carrier used to build the unlifted working-resolution
  // reference for HDR composition.
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  unsigned int output_width = 0;
  unsigned int output_height = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> composite_output;
  unsigned int composite_output_width = 0;
  unsigned int composite_output_height = 0;
  DXGI_FORMAT composite_output_format = DXGI_FORMAT_UNKNOWN;

  // Full-resolution packed HDR gain-map representation. RGB is the linear SDR
  // base; A is the linear carrier-to-HDR reconstruction multiplier. This is RGBA32F: no quantization
  // is acceptable in the round-trip path.
  Microsoft::WRL::ComPtr<ID3D12Resource> packed_hdr_sdr;
  unsigned int packed_width = 0;
  unsigned int packed_height = 0;

  // [0] = sortable min gain, [1] = sortable max gain, [2] = out-of-range or
  // SDR gamut-fitted pixel count.
  Microsoft::WRL::ComPtr<ID3D12Resource> gain_stats;
  size_t gain_stats_bytes = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> working_input;
  Microsoft::WRL::ComPtr<ID3D12Resource> working_output;
  Microsoft::WRL::ComPtr<ID3D12Resource> unlifted_working_input;
  unsigned int unlifted_working_width = 0;
  unsigned int unlifted_working_height = 0;
  DXGI_FORMAT unlifted_working_format = DXGI_FORMAT_UNKNOWN;
  // Full-resolution combined exposure used by HDR inverse compose. R32_FLOAT
  // stores global exposure × the exact local exposure factor applied by the
  // gain-map split to that source pixel.
  Microsoft::WRL::ComPtr<ID3D12Resource> effective_exposure_map;
  unsigned int effective_exposure_width = 0;
  unsigned int effective_exposure_height = 0;
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
  bool EnsurePackedOutput(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  bool EnsureGainStats(ID3D12Device* device);
  bool EnsureWorkingResources(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height);
  bool EnsureUnliftedWorkingInput(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height);
  bool EnsureEffectiveExposureMap(
      ID3D12Device* device, unsigned int width, unsigned int height);

  // Ensures scratch slot `index` matches the given size and format.
  bool EnsureScratch(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int width,
      unsigned int height,
      uint32_t index);

  void Shutdown();

  ID3D12Resource* GetOutput() const { return output.Get(); }
  // The otherwise-unused full-resolution output allocation holds the HDR
  // carrier with Local Exposure disabled, used to prepare a matching residual
  // reference at working resolution.
  ID3D12Resource* GetGlobalHdrCarrier() const { return output.Get(); }
  ID3D12Resource* GetCompositeOutput() const { return composite_output.Get(); }
  ID3D12Resource* GetPackedOutput() const { return packed_hdr_sdr.Get(); }
  ID3D12Resource* GetGainStats() const { return gain_stats.Get(); }
  ID3D12Resource* GetWorkingInput() const { return working_input.Get(); }
  ID3D12Resource* GetWorkingOutput() const { return working_output.Get(); }
  ID3D12Resource* GetUnliftedWorkingInput() const {
    return unlifted_working_input.Get();
  }
  ID3D12Resource* GetEffectiveExposureMap() const { return effective_exposure_map.Get(); }
  // True when slot `index` already holds a texture of the given shape. Lets a
  // caller pick between two slots instead of letting EnsureScratch dispose a
  // matching one in the other.
  bool ScratchMatches(unsigned int width, unsigned int height,
                      DXGI_FORMAT format, uint32_t index) const {
    return index < kScratchCount && scratch_[index] != nullptr &&
           scratch_width_[index] == width &&
           scratch_height_[index] == height &&
           scratch_format_[index] == format;
  }

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
  static bool CreateUavBuffer(
      ID3D12Device* device,
      uint64_t byte_width,
      ID3D12Resource** out_resource);
};

}  // namespace reshade_dlssnr

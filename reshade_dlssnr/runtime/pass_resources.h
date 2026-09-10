// Per-pass output, UI mask, composite, working-scale, and scratch resources.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace reshade_dlssnr {

// Generic scratch texture pool for intermediate stages (pre-exposure,
// blend helpers, etc.). Slots are created lazily at whatever size/format
// is requested and reused across frames until the requested size changes.
constexpr uint32_t kScratchCount = 4;

// The per-frame white point reduction uses a kWhitePointGridSize² tile grid.
constexpr uint32_t kWhitePointGridSize = 8;

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

  // Lazily creates the small R32_FLOAT white point reduction buffer.
  bool EnsureWhitePointBuffer(ID3D12Device* device);

  void Shutdown();

  ID3D12Resource* GetOutput() const { return output.Get(); }
  ID3D12Resource* GetCompositeOutput() const { return composite_output.Get(); }
  ID3D12Resource* GetWorkingInput() const { return working_input.Get(); }
  ID3D12Resource* GetWorkingOutput() const { return working_output.Get(); }
  ID3D12Resource* GetScratch(uint32_t index) const {
    return index < kScratchCount ? scratch_[index].Get() : nullptr;
  }
  ID3D12Resource* GetWhitePointBuffer() const {
    return white_point_buffer.Get();
  }

 private:
  Microsoft::WRL::ComPtr<ID3D12Resource> scratch_[kScratchCount];
  unsigned int scratch_width_[kScratchCount] = {};
  unsigned int scratch_height_[kScratchCount] = {};
  DXGI_FORMAT scratch_format_[kScratchCount] = {};

  Microsoft::WRL::ComPtr<ID3D12Resource> white_point_buffer;

  static bool CreateUavTexture(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      ID3D12Resource** out_resource);
};

}  // namespace reshade_dlssnr

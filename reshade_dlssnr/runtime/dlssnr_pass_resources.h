// Per-pass output, UI mask, composite, and working-scale resource ownership.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace reshade_dlssnr {

class DlssNrPassResources {
 public:
  Microsoft::WRL::ComPtr<ID3D12Resource> output;
  unsigned int output_width = 0;
  unsigned int output_height = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> ui_mask;
  unsigned int ui_mask_width = 0;
  unsigned int ui_mask_height = 0;
  DXGI_FORMAT ui_mask_format = DXGI_FORMAT_UNKNOWN;

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
  bool EnsureUiMask(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  bool EnsureCompositeOutput(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc);
  bool EnsureWorkingResources(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      unsigned int work_width,
      unsigned int work_height);

  void Shutdown();

 private:
  static bool CreateUavTexture(
      ID3D12Device* device,
      const D3D12_RESOURCE_DESC& desc,
      ID3D12Resource** out_resource);
};

}  // namespace reshade_dlssnr

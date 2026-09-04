#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <string>

#include "shaders/dlssnr_cso.h"

namespace reshade_dlssnr {

enum DlssNrMode : uint32_t {
  DlssNrMode_Encode = 0,
  DlssNrMode_Resolve = 1,
  DlssNrMode_Downsample = 2,
};

struct alignas(256) DlssNrConstants {
  uint32_t Mode = 0;
  float WhitePoint = 1.0f;

  uint32_t Width = 0;
  uint32_t Height = 0;

  float TransferStrength = 0.5f;
  float ColourStrength = 0.5f;

  uint32_t DebugView = 0;
  float MaxRatio = 4.0f;

  uint32_t Passthrough = 0;

  float MvScaleX = 1.0f;
  float MvScaleY = 1.0f;

  uint32_t GuideWidth = 0;
  uint32_t GuideHeight = 0;

  uint32_t CompareMode = 0;
  float CompareSplit = 0.5f;

  float CompareZoom = 1.0f;
  uint32_t CompareSwap = 0;
};
static_assert(sizeof(DlssNrConstants) == 256, "Constants must have a 256-byte CBV allocation size");

constexpr uint32_t kDlssNrHeapCount = 16;
constexpr uint32_t kDlssNrSrvCount = 5;
constexpr uint32_t kDlssNrUavCount = 2;
constexpr uint32_t kDlssNrCbvCount = 1;
constexpr uint32_t kDlssNrPerDispatchDescriptors = kDlssNrSrvCount + kDlssNrUavCount + kDlssNrCbvCount;

class DlssNrCodec {
 public:
  bool Init(ID3D12Device* device);

  bool InitMaskShader(ID3D12Device* device);

  bool Dispatch(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* source,
      ID3D12Resource* model,
      ID3D12Resource* original,
      ID3D12Resource* motion,
      ID3D12Resource* prev_edit,
      ID3D12Resource* out_target,
      ID3D12Resource* out_keep);

  bool DispatchMask(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* pre_ui,
      ID3D12Resource* backbuffer,
      ID3D12Resource* out_mask);

  void Destroy();

  bool IsInit() const { return device_ != nullptr; }

 private:
  ID3D12Device* device_ = nullptr;
  ID3D12RootSignature* root_signature_ = nullptr;
  ID3D12PipelineState* pipeline_state_ = nullptr;
  ID3D12PipelineState* mask_pipeline_state_ = nullptr;

  ID3D12DescriptorHeap* descriptor_heap_ = nullptr;
  uint32_t descriptor_size_ = 0;
  uint32_t heap_index_ = 0;

  ID3D12Resource* constant_buffers_[kDlssNrHeapCount] = {};

  void CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateConstantBufferView(ID3D12Resource* resource, const void* data, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  DXGI_FORMAT TypedFormat(DXGI_FORMAT format) const;
};

}  // namespace reshade_dlssnr

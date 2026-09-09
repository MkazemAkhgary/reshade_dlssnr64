// DLSSNR compute codec: root signature, descriptor heap, shader dispatch API.
#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>


namespace reshade_dlssnr {

enum DlssNrMode : uint32_t {
  DlssNrMode_Encode = 0,
  DlssNrMode_Resolve = 1,
  DlssNrMode_Downsample = 2,
};

struct alignas(256) DlssNrConstants {
  uint32_t Mode = 0;

  uint32_t Width = 0;
  uint32_t Height = 0;

  float TransferStrength = 0.5f;
  float LumaStrength = 1.0f;
  float ChromaStrength = 1.0f;

  uint32_t DebugView = 0;
  float UiMaskThreshold = 0.02f;
  float UiMaskSoftness = 0.06f;

  float ResidualSuppression = 0.85f;
  float ResidualThreshold = 0.10f;

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

  bool DispatchMask(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* pre_ui,
      ID3D12Resource* backbuffer,
      ID3D12Resource* out_mask);

  bool InitCompositeShader(ID3D12Device* device);

  bool DispatchComposite(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* enhanced,
      ID3D12Resource* original,
      ID3D12Resource* mask,
      ID3D12Resource* out_target);

  bool InitLumaColorShader(ID3D12Device* device);

  bool DispatchLumaColor(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* enhanced,
      ID3D12Resource* original,
      ID3D12Resource* out_target);

  bool InitDebugResidualShader(ID3D12Device* device);

  bool DispatchDebugResidual(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* enhanced,
      ID3D12Resource* original,
      ID3D12Resource* out_target);

  bool InitDownsampleShader(ID3D12Device* device);

  bool DispatchDownsample(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* source,
      ID3D12Resource* out_target);

  bool InitUpsampleResidualShader(ID3D12Device* device);

  bool DispatchUpsampleResidual(
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* original,
      ID3D12Resource* low_input,
      ID3D12Resource* low_enhanced,
      ID3D12Resource* out_target);

  void Destroy();

  bool IsInit() const { return device_ != nullptr; }

 private:
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> mask_pipeline_state_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> composite_pipeline_state_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> luma_color_pipeline_state_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> debug_residual_pipeline_state_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> downsample_pipeline_state_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> upsample_residual_pipeline_state_;

  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
  uint32_t descriptor_size_ = 0;
  uint32_t heap_index_ = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffers_[kDlssNrHeapCount];

  bool CreateRootSignature(ID3D12Device* device);
  bool CreateComputePipelineState(
      ID3D12Device* device,
      const char* shader_source,
      ID3D12PipelineState** out_pipeline_state);
  void CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateConstantBufferView(ID3D12Resource* resource, const void* data, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  DXGI_FORMAT TypedFormat(DXGI_FORMAT format) const;

  bool BindAndDispatch(
      ID3D12GraphicsCommandList* command_list,
      ID3D12PipelineState* pipeline_state,
      const DlssNrConstants& constants,
      ID3D12Resource* const* srvs,
      ID3D12Resource* const* uavs);
};

}  // namespace reshade_dlssnr

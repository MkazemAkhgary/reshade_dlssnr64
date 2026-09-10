// DLSSNR compute codec: root signature, descriptor heap, generic dispatch.
#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace reshade_dlssnr {

struct alignas(256) DlssNrConstants {
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;

  // Dynamic shader parameters, indexed by kShaderParams order.
  // Must align with HLSL float4 gParams[4] (16-byte boundary).
  float params[16] = {};
};
static_assert(sizeof(DlssNrConstants) == 256, "Constants must have a 256-byte CBV allocation size");

constexpr uint32_t kDlssNrHeapCount = 16;
constexpr uint32_t kDlssNrSrvCount = 3;
constexpr uint32_t kDlssNrUavCount = 1;
constexpr uint32_t kDlssNrCbvCount = 1;
constexpr uint32_t kDlssNrPerDispatchDescriptors = kDlssNrSrvCount + kDlssNrUavCount + kDlssNrCbvCount;

class DlssNrCodec {
 public:
  bool Init(ID3D12Device* device);

  // Dispatches a registered compute pass by name. The pass is lazily
  // compiled the first time it is used. SRV/UAV layouts are fixed at
  // kDlssNrSrvCount / kDlssNrUavCount.
  bool Dispatch(
      const char* pass_name,
      ID3D12GraphicsCommandList* command_list,
      const DlssNrConstants& constants,
      ID3D12Resource* const* srvs,
      ID3D12Resource* const* uavs);

  void Destroy();

  bool IsInit() const { return device_ != nullptr; }

 private:
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
  uint32_t descriptor_size_ = 0;
  uint32_t heap_index_ = 0;

  Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffers_[kDlssNrHeapCount];

  struct CachedPass {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  };
  std::unordered_map<std::string, CachedPass> passes_;

  bool CreateRootSignature(ID3D12Device* device);
  bool CompilePipeline(const char* hlsl, ID3D12PipelineState** out_pso);
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

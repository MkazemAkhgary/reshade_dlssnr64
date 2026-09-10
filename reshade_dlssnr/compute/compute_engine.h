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

struct alignas(256) ComputeConstants {
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;

  // Dynamic shader parameters, indexed by kShaderParams order.
  // Must align with HLSL float4 gParams[4] (16-byte boundary).
  float params[16] = {};
};
static_assert(sizeof(ComputeConstants) == 256, "Constants must have a 256-byte CBV allocation size");

// Descriptor layout contract for every compute pass.
//
// The single root descriptor table (root param 0) exposes, in order:
//   - kSrvCount SRVs   at shader registers t0..t(kSrvCount-1)
//   - kUavCount UAVs   at shader registers u0..u(kUavCount-1)
//   - kCbvCount CBV    at shader register b0
//   - one static linear-clamp sampler at s0
//
// Every Dispatch() call MUST provide exactly kSrvCount SRVs and
// kUavCount UAVs. Passing fewer leaves stale descriptors from the
// previous dispatch in the unused slots. Shaders that do not declare a
// register simply ignore that slot, so it is safe (and required) to fill
// unused slots with any valid resource — typically the same texture or
// output resource repeated.
//
// If a pass needs more bindings, increase the count here; the root
// signature, descriptor heap, and BindAndDispatch loop scale with it
// automatically. Grid size is derived from constants.Width/Height using
// a fixed 8x8 thread group (see BindAndDispatch).
constexpr uint32_t kDescriptorRingSize = 16;
constexpr uint32_t kSrvCount = 4;
constexpr uint32_t kUavCount = 1;
constexpr uint32_t kCbvCount = 1;
constexpr uint32_t kDescriptorsPerDispatch = kSrvCount + kUavCount + kCbvCount;

class ComputeEngine {
 public:
  bool Init(ID3D12Device* device);

  // Dispatches a registered compute pass by name. The pass is lazily
  // compiled the first time it is used.
  //
  // `srvs` must point to an array of exactly kSrvCount valid
  // resources (t0..tN-1). `uavs` must point to an array of exactly
  // kUavCount valid resources (u0..uN-1). Fill unused slots by
  // repeating an already-bound resource. See the descriptor layout
  // contract above for details.
  bool Dispatch(
      const char* pass_name,
      ID3D12GraphicsCommandList* command_list,
      const ComputeConstants& constants,
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

  Microsoft::WRL::ComPtr<ID3D12Resource> constant_buffers_[kDescriptorRingSize];

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
      const ComputeConstants& constants,
      ID3D12Resource* const* srvs,
      ID3D12Resource* const* uavs);
};

}  // namespace reshade_dlssnr

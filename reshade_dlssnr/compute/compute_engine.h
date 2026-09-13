// DLSSNR compute codec: root signature, descriptor heap, generic dispatch.
#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace reshade_dlssnr {

struct alignas(256) ComputeConstants {
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;

  // Dynamic shader parameters, indexed by kShaderParams order.
  // Must align with HLSL `float4 gParams[12]` (16-byte boundary). Each
  // parameter occupies one float; four parameters fill one float4. 48
  // floats leaves headroom for future effects without ever hitting the
  // 256-byte CBV ceiling (16-byte header + 48*4 = 208 bytes used).
  float params[48] = {};
};
static_assert(sizeof(ComputeConstants) <= 256, "Constants must fit in a 256-byte CBV");

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
// Resources may be 2D textures or buffers. Buffer views are created as
// typed R32_UINT (HLSL `Buffer<uint>` / `RWBuffer<uint>`), so a buffer of
// N 32-bit elements has `Width = N*4` in bytes. This lets a pass use
// atomics (`InterlockedAdd`) for accumulation — e.g. histograms.
//
// If a pass needs more bindings, increase the count here; the root
// signature, descriptor heap, and BindAndDispatch loop scale with it
// automatically. Grid size is derived from constants.Width/Height using
// a fixed 8x8 thread group (see BindAndDispatch).
constexpr uint32_t kDescriptorRingSize = 16;
constexpr uint32_t kSrvCount = 3;
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
  //
  // By default the thread-group grid is derived from constants.Width/Height
  // assuming an 8×8 thread group (matches the built-in passes). Pass
  // non-zero `threads_x`/`threads_y` to override — useful for passes that
  // only need a fixed grid regardless of frame size (e.g. a reduction).
  bool Dispatch(
      const char* pass_name,
      ID3D12GraphicsCommandList* command_list,
      const ComputeConstants& constants,
      ID3D12Resource* const* srvs,
      ID3D12Resource* const* uavs,
      uint32_t threads_x = 0,
      uint32_t threads_y = 0,
      uint32_t threads_z = 0);

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
  // Guards `passes_`, which is filled by the worker and read by Dispatch.
  // `device_` and `root_signature_` are set before the worker starts and
  // reset after it joins, so they need no additional protection.
  std::mutex passes_mutex_;
  std::unordered_map<std::string, CachedPass> passes_;

  std::thread worker_;
  bool worker_stop_ = false;

  // Compiles every pass in kComputePasses[] off the render thread. Runs
  // once per Init; joins before any device or root signature teardown.
  void WorkerCompileAll();

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
      ID3D12Resource* const* uavs,
      uint32_t threads_x,
      uint32_t threads_y,
      uint32_t threads_z);
};

}  // namespace reshade_dlssnr

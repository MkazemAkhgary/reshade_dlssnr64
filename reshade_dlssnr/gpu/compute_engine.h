// DLSSNR compute codec: root signature, descriptor heap, generic dispatch.
#pragma once

#include <d3d12.h>
#include <d3dx12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "compute_constants.h"
#include "project_constants.h"

namespace reshade_dlssnr {

struct ComputePassDef;

// Binding contract: shaders/common/registers.hlsli.
// Register assignment code is implemented in CreateRootSignature/BindAndDispatch.

// Ring depth for the per-dispatch descriptor and constant slots.
//
// Both are written by the CPU while the command is recorded and read by the
// GPU when it executes, so a slot may only be rewritten once the dispatch that
// used it has run. The passes record onto the *game's* command list, which is
// submitted whenever the game gets round to it, so the ring has to cover every
// frame the CPU can be ahead of the GPU.
//
// At 16 slots it covered barely two frames' worth: two passes fitted (6
// dispatches per frame, 8 with the HDR histogram) and three did not (9 and 11),
// so the third pass made the ring overwrite constants a pending dispatch was
// still going to read.
constexpr uint32_t kMaxFramesInFlight = 16;  // DXGI's maximum swapchain buffers
// The ring covers the prior twelve per-pass dispatches plus up to seven
// temporal edit/reprojection/validation/history dispatches, one game-exposure
// read, HDR histogram work, Release grading, and Importance Mask. Descriptors
// and constants are consumed asynchronously by the GPU.
constexpr uint32_t kMaxDispatchesPerFrame = kMaxPasses * 19 + 10 + 3;
constexpr uint32_t kDescriptorRingSize =
    kMaxFramesInFlight * kMaxDispatchesPerFrame;

constexpr uint32_t kSrvCount = 4;
constexpr uint32_t kUavCount = 1;
constexpr uint32_t kCbvCount = 1;
constexpr uint32_t kDescriptorsPerDispatch = kSrvCount + kUavCount + kCbvCount;

class ComputeEngine {
 public:
  bool Init(ID3D12Device* device);

  // Dispatches a registered compute pass by name. Passes are normally
  // compiled by the background worker; a post-ready miss gets one fallback
  // compile attempt and then is remembered as failed.
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

  // Stops and joins the background compile.
  //
  // A std::thread destroyed while still joinable calls std::terminate, so this
  // cannot be left to the caller: AddonController is a function-local static
  // whose destructor runs at process exit, and it is reachable without
  // Shutdown() having run. Destroy() is idempotent, so joining here as well is
  // free.
  ~ComputeEngine();

  void Destroy();

  bool IsInit() const { return device_ != nullptr; }

  // True once the background compiler has finished its pass over
  // kComputePasses[]. Callers check this before dispatching so the render
  // thread never blocks on a compile; if it is false, they skip the frame
  // without treating it as a failure. Dispatch itself also refuses to
  // compile synchronously while this is false, so an unguarded caller
  // cannot reintroduce the hitch.
  bool IsReady() const { return ready_.load(std::memory_order_acquire); }

  // Query the background compiler result for one pass without triggering a
  // synchronous compile. Used by optional stages that may legitimately wait
  // for the shader worker during startup.
  bool IsPassCompiled(const char* pass_name);
  bool IsPassFailed(const char* pass_name);

 private:
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
  uint32_t descriptor_size_ = 0;
  uint32_t heap_index_ = 0;

  // One upload buffer holding every dispatch slot's 256-byte constants. A
  // single allocation avoids a committed resource per dispatch while the
  // descriptor/constant ring is intentionally deep.
  static constexpr uint32_t kConstantStride = 256;  // CBV alignment requirement
  Microsoft::WRL::ComPtr<ID3D12Resource> constants_;

  struct CachedPass {
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
  };
  // Guards `passes_`, which is filled by the worker and read by Dispatch.
  // `device_` and `root_signature_` are set before the worker starts and
  // reset after it joins, so they need no additional protection.
  std::mutex passes_mutex_;
  std::unordered_map<std::string, CachedPass> passes_;
  // Passes that were attempted by the background compiler but could not
  // produce a usable pipeline. Remembering them prevents a bad shader from
  // being recompiled and logged once per frame after IsReady() becomes true.
  std::unordered_set<std::string> failed_passes_;

  // Compiler diagnostics are deduplicated independently of pass state because
  // both the worker and a post-ready fallback compile can surface the same
  // warning/error. The key includes the pass name, severity and exact text.
  std::mutex diagnostic_mutex_;
  std::unordered_set<std::string> logged_shader_diagnostics_;

  std::thread worker_;
  bool worker_stop_ = false;

  // Set by the worker when it finishes, cleared by Init before it starts
  // a fresh worker. Acquire/release only; the heavier state (passes_) is
  // still guarded by passes_mutex_.
  std::atomic<bool> ready_{false};

  // Compiles every pass in kComputePasses[] off the render thread. Runs
  // once per Init; joins before any device or root signature teardown.
  void WorkerCompileAll();

  bool CreateRootSignature(ID3D12Device* device);
  bool CompilePipeline(const ComputePassDef& def,
                       ID3D12PipelineState** out_pso);
  void CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void CreateConstantBufferView(ID3D12Resource* resource, uint64_t offset,
                                const void* data, size_t size,
                                D3D12_CPU_DESCRIPTOR_HANDLE handle);
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

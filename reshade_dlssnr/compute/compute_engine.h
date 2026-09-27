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

#include "shader_params.h"

namespace reshade_dlssnr {

// ---------------------------------------------------------------------------
// ComputeConstants::params region map
// ---------------------------------------------------------------------------
//
// `params` is one flat float array, and a single dispatch can have more than
// one consumer writing it: ComposeModelOutputHdr receives both the
// kShaderParams table (copied in by the caller) and the HDR uniforms. Those
// shared regions are pinned here rather than left to each caller's literal
// indices, because the previous arrangement — the table filling [0, N) and
// the HDR uniforms hardcoded at [8..12] — was safe only while N was 7. The
// eighth parameter would have overwritten the exposure with no diagnostic
// anywhere: the shader still compiles, the dispatch still succeeds, and the
// image is simply wrong.
//
// Passes whose parameters are theirs alone — DebugDisplay, TonemapForward,
// the histogram passes — keep their own layout starting at 0. Nothing else
// writes those slots in the same dispatch, so they need no region.
//
//   [0,  kShaderParamCount)  the kShaderParams table (per-pass sliders)
//   [8,  24)                 the colour grade block
//   [24, 32)                 the residual edit block
//   [32, 36)                 the HDR uniforms
//   [36, 48)                 the FSR EASU constants
//   [48, 52)                 the FSR RCAS constants
//
// Gaps between regions are deliberate headroom, not waste: they let a region
// grow a little before anything else has to move.
//
// So: a consumer that shares a dispatch takes a base from here, and the
// asserts below prove the regions cannot overlap or run off the end.
constexpr uint32_t kShaderParamSlotBase = 0;
constexpr uint32_t kGradeSlotBase = 8;
constexpr uint32_t kGradeSlotCount = 16;
constexpr uint32_t kResidualSlotBase = 24;
constexpr uint32_t kResidualSlotCount = 8;
constexpr uint32_t kHdrSlotBase = 32;
constexpr uint32_t kHdrSlotCount = 4;
constexpr uint32_t kEasuSlotBase = 36;
constexpr uint32_t kEasuSlotCount = 12;
constexpr uint32_t kRcasSlotBase = 48;
constexpr uint32_t kRcasSlotCount = 4;

constexpr uint32_t kParamSlotCount = 52;

static_assert(kShaderParamCount <= kGradeSlotBase,
              "the kShaderParams table has grown into the grade block - move "
              "kGradeSlotBase up");
static_assert(kGradeSlotBase + kGradeSlotCount <= kResidualSlotBase,
              "the grade block has grown into the residual block - move "
              "kResidualSlotBase up");
static_assert(kResidualSlotBase + kResidualSlotCount <= kHdrSlotBase,
              "the residual block has grown into the HDR uniforms - move "
              "kHdrSlotBase up");
static_assert(kHdrSlotBase + kHdrSlotCount <= kEasuSlotBase,
              "the HDR uniforms have grown into the EASU constants - move "
              "kEasuSlotBase up");
static_assert(kEasuSlotBase + kEasuSlotCount <= kRcasSlotBase,
              "the EASU constants have grown into the RCAS block - move "
              "kRcasSlotBase up");
static_assert(kRcasSlotBase + kRcasSlotCount <= kParamSlotCount,
              "the RCAS constants have grown past the end of params");

// Size and alignment are pinned to 256 bytes for two reasons: the CBV
// created in BindAndDispatch hardcodes SizeInBytes = 256, and the D3D12
// root signature requires CBVs to be 256-byte aligned. Growing `params`
// past 48 floats is fine until the struct exceeds 256 bytes, at which
// point the static_assert below fires.
struct alignas(256) ComputeConstants {
  uint32_t Width = 0;
  uint32_t Height = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;

  // Dynamic shader parameters. Consumers write one region of this array
  // rather than the whole thing; the region map is at the top of this header.
  //
  // Must align with HLSL `float4 gParams[12]`, i.e. 48 floats: one float per
  // parameter, four parameters per float4. 48 floats leaves headroom for
  // future effects without reaching the 256-byte CBV ceiling (16-byte header
  // + 48*4 = 208 bytes used).
  float params[kParamSlotCount] = {};
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
// Worst case for one frame: every pass records a tonemap, two grades, a
// compose and a debug view, and the HDR histogram adds two more. The
// static_assert in pass_session.cpp ties the literal to kMaxPasses.
constexpr uint32_t kMaxDispatchesPerFrame = 3 * 5 + 2;
constexpr uint32_t kDescriptorRingSize =
    kMaxFramesInFlight * kMaxDispatchesPerFrame;

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

 private:
  ID3D12Device* device_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptor_heap_;
  uint32_t descriptor_size_ = 0;
  uint32_t heap_index_ = 0;

  // One upload buffer holding every slot's constants, one kConstantStride
  // block per slot. A single allocation rather than one resource per slot,
  // because a committed resource costs 64 KB whatever its size and the ring is
  // deep on purpose.
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
  bool CompilePipeline(const char* hlsl, ID3D12PipelineState** out_pso);
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

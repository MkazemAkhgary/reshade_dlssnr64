// N-frame-delayed GPU -> CPU readback of a small buffer (statistics,
// histograms, exposure probes). The reader sees frame N-K's data at frame
// N, so the CPU never blocks on the GPU for the read path. The GPU copy
// into the staging buffer is queued on a command list and gated by a fence
// the caller controls (usually CommandContext's own fence).
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>

namespace reshade_dlssnr {

class GpuReadback {
 public:
  // Number of in-flight frames. A larger ring tolerates a slower CPU without
  // dropping updates, at the cost of memory and added latency.
  static constexpr uint32_t kRingSize = 3;

  // Allocates the ring of staging buffers. Safe to call repeatedly; only
  // reallocates when the byte size changes.
  bool Ensure(ID3D12Device* device, size_t bytes_per_slot);
  void Shutdown();

  // Records a copy of `source` (source_bytes) into the ring's next slot on
  // `command_list`. `fence`/`fence_value` identify the fence signal that
  // will complete after the copy — the caller must signal that value on
  // `fence` after submitting the command list (typically via
  // CommandContext::WaitGpu, whose next value is exactly what
  // CommandContext::GetNextFenceValue() returns).
  //
  // Returns true if the copy was recorded. Returns false if `source_bytes`
  // exceeds the slot size or the target slot is still pending on the GPU —
  // in that case the frame's update is simply skipped.
  bool EnqueueCopy(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* source,
      uint32_t source_bytes,
      ID3D12Fence* fence,
      uint64_t fence_value);

  // Returns a mapped read-only pointer to the most recent completed copy
  // since the last successful TryGet, or nullptr if none is ready. The
  // pointer stays valid until the next EnqueueCopy that reuses the slot.
  // Each completed fence value is returned at most once. `out_bytes`
  // receives the size of the copy that produced this data (which may be
  // smaller than the slot capacity).
  const void* TryGet(ID3D12Fence* fence, uint32_t* out_bytes);

 private:
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    void* mapped = nullptr;
    uint64_t fence_value = 0;
    uint32_t bytes = 0;  // size of the last copy recorded into this slot
    uint64_t sequence = 0;
    bool valid = false;
  };

  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  size_t bytes_per_slot_ = 0;
  Slot slots_[kRingSize];
  uint32_t next_write_ = 0;
  uint64_t write_sequence_ = 0;
  uint64_t last_consumed_sequence_ = 0;
};

}  // namespace reshade_dlssnr

// D3D12 command allocator/list/fence ownership and barrier/submit helpers.
#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace reshade_dlssnr {

class CommandContext {
 public:
  bool Ensure(ID3D12Device* device);

  // Starts a fresh recording after safely discarding any abandoned recording
  // from a skipped/failed frame.
  void Reset();
  // Closes and discards an in-progress recording without executing it. Safe
  // to call after a stage exits early; a subsequent Reset can reuse the
  // allocator/list pair without tripping D3D12 lifetime validation.
  bool Abort();
  // Clears tracked resource states. Call when resources are recreated or
  // when the tracked state may no longer reflect the GPU's actual state.
  void ClearState();
  void CloseAndSubmit(ID3D12CommandQueue* command_queue);
  // Queue a fence signal without blocking the CPU. The current command-list
  // slot can be reused only after its associated value completes.
  uint64_t SignalGpu(ID3D12CommandQueue* command_queue);
  // Synchronous fence for callers that genuinely need completion now.
  void WaitGpu(ID3D12CommandQueue* command_queue);
  uint64_t LastSignaledFenceValue() const { return last_signaled_fence_value_; }
  uint64_t CompletedFenceValue() const;

  // Auto-tracked transition: uses tracked state (or resource's initial state
  // from its descriptor if not yet tracked) and transitions to `after`.
  void Transition(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES after);

  // Copies a source texture into the swapchain backbuffer, handling the
  // common COPY_SOURCE/COPY_DEST/PRESENT/UNORDERED_ACCESS transitions.
  void CopyResourceToBackbuffer(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* source,
      ID3D12Resource* backbuffer);

  ID3D12GraphicsCommandList* GetCommandList() const { return frames_[current_frame_].command_list.Get(); }

  void SetResourceState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

  // Returns the tracked state of a resource, or COMMON if untracked.
  D3D12_RESOURCE_STATES GetResourceState(ID3D12Resource* resource) const;

  // Inserts an explicit UAV barrier on `resource`. Use after external code
  // (e.g. NVSDK_NGX EvaluateFeature) may have written to a resource we own
  // through a UAV, before we read it. A state transition alone is not
  // guaranteed to synchronize UAV writes.
  void UavBarrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource);

  // The internal fence used for queued work and optional synchronous waits.
  // Only valid after Ensure().
  ID3D12Fence* GetFence() const { return fence_.Get(); }

  // The next value SignalGpu() will enqueue on this queue-ordered fence.
  // Consumers may attach queued GPU work to that value without a CPU wait.
  uint64_t GetNextFenceValue() const { return fence_value_ + 1; }

  void Shutdown();

 private:
  // The command queue may be several frames ahead of GPU completion. Keep a
  // command allocator/list per possible swapchain frame and wait only when a
  // particular slot is reused, never immediately after every Present.
  static constexpr size_t kCommandRingSize = 16;
  struct FrameCommands {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    UINT64 fence_value = 0;
  };

  bool WaitForFenceValue(UINT64 value);

  std::array<FrameCommands, kCommandRingSize> frames_;
  size_t current_frame_ = 0;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = INVALID_HANDLE_VALUE;
  UINT64 fence_value_ = 0;
  UINT64 last_signaled_fence_value_ = 0;
  bool recording_ = false;
  bool submitted_since_reset_ = false;

  std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resource_states_;
};

}  // namespace reshade_dlssnr

// D3D12 command allocator/list/fence ownership and barrier/submit helpers.
#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <unordered_map>

namespace reshade_dlssnr {

class DlssNrCommandContext {
 public:
  bool Ensure(ID3D12Device* device);

  void Reset();
  void CloseAndSubmit(ID3D12CommandQueue* command_queue);
  void WaitGpu(ID3D12CommandQueue* command_queue);

  // Backward-compatible barrier: transitions a resource and updates tracking.
  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);

  // Auto-tracked transition: uses tracked state (or resource's initial state
  // from its descriptor if not yet tracked) and transitions to `after`.
  void Transition(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES after);

  ID3D12GraphicsCommandList* GetCommandList() const { return command_list_.Get(); }

  void SetResourceState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

  void Shutdown();

 private:
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  HANDLE fence_event_ = INVALID_HANDLE_VALUE;
  UINT64 fence_value_ = 0;

  std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> resource_states_;
};

}  // namespace reshade_dlssnr

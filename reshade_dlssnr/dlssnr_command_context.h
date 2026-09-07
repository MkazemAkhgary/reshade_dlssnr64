#pragma once

#include <d3d12.h>
#include <windows.h>

#include "dlss_guides.h"

namespace reshade_dlssnr {

class DlssNrCommandContext {
 public:
  bool Ensure(ID3D12Device* device);

  void Reset();
  void CloseAndSubmit(ID3D12CommandQueue* command_queue);
  void WaitGpu(ID3D12CommandQueue* command_queue);

  void Barrier(
      ID3D12GraphicsCommandList* command_list,
      ID3D12Resource* resource,
      D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after);

  ID3D12GraphicsCommandList* GetCommandList() const { return command_list_; }

  void Shutdown();

 private:
  ID3D12CommandAllocator* command_allocator_ = nullptr;
  ID3D12GraphicsCommandList* command_list_ = nullptr;
  ID3D12Fence* fence_ = nullptr;
  HANDLE fence_event_ = INVALID_HANDLE_VALUE;
  UINT64 fence_value_ = 0;
};

}  // namespace reshade_dlssnr

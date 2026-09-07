#include "dlssnr_command_context.h"

namespace reshade_dlssnr {

bool DlssNrCommandContext::Ensure(ID3D12Device* device) {
  if (command_allocator_ != nullptr) return true;
  if (device == nullptr) return false;

  if (FAILED(device->CreateCommandAllocator(
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&command_allocator_)))) {
    return false;
  }

  if (FAILED(device->CreateCommandList(
          0,
          D3D12_COMMAND_LIST_TYPE_DIRECT,
          command_allocator_,
          nullptr,
          IID_PPV_ARGS(&command_list_)))) {
    command_allocator_->Release();
    command_allocator_ = nullptr;
    return false;
  }

  command_list_->Close();

  if (FAILED(device->CreateFence(
          0,
          D3D12_FENCE_FLAG_NONE,
          IID_PPV_ARGS(&fence_)))) {
    command_list_->Release();
    command_list_ = nullptr;
    command_allocator_->Release();
    command_allocator_ = nullptr;
    return false;
  }

  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    fence_->Release();
    fence_ = nullptr;
    command_list_->Release();
    command_list_ = nullptr;
    command_allocator_->Release();
    command_allocator_ = nullptr;
    return false;
  }

  return true;
}

void DlssNrCommandContext::Reset() {
  if (command_allocator_ != nullptr && command_list_ != nullptr) {
    command_allocator_->Reset();
    command_list_->Reset(command_allocator_, nullptr);
  }
}

void DlssNrCommandContext::CloseAndSubmit(ID3D12CommandQueue* command_queue) {
  if (command_list_ == nullptr || command_queue == nullptr) return;

  command_list_->Close();
  ID3D12CommandList* lists[] = {command_list_};
  command_queue->ExecuteCommandLists(1, lists);
}

void DlssNrCommandContext::WaitGpu(ID3D12CommandQueue* command_queue) {
  if (command_queue == nullptr || fence_ == nullptr) return;

  const UINT64 value = ++fence_value_;
  command_queue->Signal(fence_, value);

  if (fence_->GetCompletedValue() < value) {
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }

  DlssGuides::Get().ReleaseRetiredGameResources();
}

void DlssNrCommandContext::Barrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
  if (before == after || resource == nullptr || command_list == nullptr) return;

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);
}

void DlssNrCommandContext::Shutdown() {
  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }

  if (fence_ != nullptr) {
    fence_->Release();
    fence_ = nullptr;
  }

  if (command_list_ != nullptr) {
    command_list_->Release();
    command_list_ = nullptr;
  }

  if (command_allocator_ != nullptr) {
    command_allocator_->Release();
    command_allocator_ = nullptr;
  }

  fence_value_ = 0;
}

}  // namespace reshade_dlssnr

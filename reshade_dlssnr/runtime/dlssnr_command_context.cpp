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
          command_allocator_.Get(),
          nullptr,
          IID_PPV_ARGS(&command_list_)))) {
    command_allocator_.Reset();
    return false;
  }

  command_list_->Close();

  if (FAILED(device->CreateFence(
          0,
          D3D12_FENCE_FLAG_NONE,
          IID_PPV_ARGS(&fence_)))) {
    command_list_.Reset();
    command_allocator_.Reset();
    return false;
  }

  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    fence_.Reset();
    command_list_.Reset();
    command_allocator_.Reset();
    return false;
  }

  return true;
}

void DlssNrCommandContext::Reset() {
  if (command_allocator_ != nullptr && command_list_ != nullptr) {
    command_allocator_->Reset();
    command_list_->Reset(command_allocator_.Get(), nullptr);
  }

  resource_states_.clear();
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

  resource_states_[resource] = after;
}

void DlssNrCommandContext::Transition(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES after) {
  if (resource == nullptr || command_list == nullptr) return;

  auto it = resource_states_.find(resource);
  D3D12_RESOURCE_STATES before;

  if (it != resource_states_.end()) {
    before = it->second;
  } else {
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    switch (desc.Dimension) {
      case D3D12_RESOURCE_DIMENSION_BUFFER:
        before = D3D12_RESOURCE_STATE_GENERIC_READ;
        break;
      case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0) {
          before = D3D12_RESOURCE_STATE_RENDER_TARGET;
        } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0) {
          before = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        } else if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
          before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        } else {
          before = D3D12_RESOURCE_STATE_COMMON;
        }
        break;
      default:
        before = D3D12_RESOURCE_STATE_COMMON;
        break;
    }
  }

  if (before == after) {
    resource_states_[resource] = after;
    return;
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);

  resource_states_[resource] = after;
}

void DlssNrCommandContext::Shutdown() {
  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }

  fence_.Reset();
  command_list_.Reset();
  command_allocator_.Reset();

  fence_value_ = 0;
}

}  // namespace reshade_dlssnr

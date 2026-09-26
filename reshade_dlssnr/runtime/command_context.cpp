#include "command_context.h"

#include <cstdio>

#include <reshade.hpp>

#include "d3d12_utils.h"
#include "pipeline_trace.h"

namespace reshade_dlssnr {

namespace {

// Most D3D12 entry points return void, so a rejected call leaves no trace.
// Logged once per distinct failure; a removed device stays removed.
void LogFailureOnce(const char* what, HRESULT result) {
  if (SUCCEEDED(result)) return;

  static const char* reported[8] = {};
  static int reported_count = 0;
  for (int i = 0; i < reported_count; ++i) {
    if (reported[i] == what) return;
  }
  if (reported_count >= 8) return;
  reported[reported_count++] = what;

  char message[192];
  std::snprintf(message, sizeof(message),
                "DLSSNR command context: %s failed with 0x%08lX", what,
                static_cast<unsigned long>(result));
  reshade::log::message(reshade::log::level::error, message);
}

}  // namespace

bool CommandContext::Ensure(ID3D12Device* device) {
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

void CommandContext::Reset() {
  if (command_allocator_ != nullptr && command_list_ != nullptr) {
    // Both of these fail rather than corrupt anything when the allocator is
    // still referenced by work the GPU has not finished -- which is a bug on
    // our side, and one that shows up as a removed device several frames
    // later rather than here.
    LogFailureOnce("command allocator reset", command_allocator_->Reset());
    LogFailureOnce("command list reset",
                   command_list_->Reset(command_allocator_.Get(), nullptr));
  }
}

void CommandContext::ClearState() {
  resource_states_.clear();
}

void CommandContext::CloseAndSubmit(ID3D12CommandQueue* command_queue) {
  if (command_list_ == nullptr || command_queue == nullptr) return;

  LogFailureOnce("command list close", command_list_->Close());

  ID3D12CommandList* lists[] = {command_list_.Get()};
  command_queue->ExecuteCommandLists(1, lists);
  TRACE_CMD(this, "close_and_submit queue=%d", trace::ResourceId(command_queue));

  // ExecuteCommandLists returns void, so the only way to learn that it was
  // rejected is to ask the device afterwards. This is the call that turns an
  // invalid command into a removed device, so it is the one worth watching.
  ID3D12Device* device = nullptr;
  if (SUCCEEDED(command_list_->GetDevice(IID_PPV_ARGS(&device))) &&
      device != nullptr) {
    LogFailureOnce("device removed after execute_command_lists",
                   device->GetDeviceRemovedReason());
    device->Release();
  }
}

void CommandContext::WaitGpu(ID3D12CommandQueue* command_queue) {
  if (command_queue == nullptr || fence_ == nullptr) return;

  const UINT64 value = ++fence_value_;
  command_queue->Signal(fence_.Get(), value);
  TRACE_CMD(this, "wait_gpu signal=%llu queue=%d",
            static_cast<unsigned long long>(value),
            trace::ResourceId(command_queue));

  if (fence_->GetCompletedValue() < value) {
    fence_->SetEventOnCompletion(value, fence_event_);
    WaitForSingleObject(fence_event_, INFINITE);
  }
  TRACE_CMD(this, "wait_gpu done signal=%llu",
            static_cast<unsigned long long>(value));
}

void CommandContext::Transition(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES after) {
  if (resource == nullptr || command_list == nullptr) return;

  auto it = resource_states_.find(resource);
  const bool tracked = it != resource_states_.end();
  const D3D12_RESOURCE_STATES before =
      tracked ? it->second : InferResourceState(resource);

  if (before == after) {
    resource_states_[resource] = after;
    TRACE_CMD(this, "transition res=%d(%s) %s SKIP(same) tracked=%d",
              trace::ResourceId(resource),
              trace::ResourceNameOr(resource, "-"),
              trace::StateName(before), tracked ? 1 : 0);
    return;
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  command_list->ResourceBarrier(1, &barrier);

  TRACE_CMD(this, "transition res=%d(%s) %s tracked=%d",
            trace::ResourceId(resource),
            trace::ResourceNameOr(resource, "-"),
            trace::StatePairName(before, after), tracked ? 1 : 0);

  resource_states_[resource] = after;
}

void CommandContext::SetResourceState(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES state) {
  if (resource == nullptr) return;
  resource_states_[resource] = state;
  TRACE_CMD(this, "seed res=%d(%s) = %s", trace::ResourceId(resource),
            trace::ResourceNameOr(resource, "-"), trace::StateName(state));
}

D3D12_RESOURCE_STATES CommandContext::GetResourceState(
    ID3D12Resource* resource) const {
  if (resource == nullptr) return D3D12_RESOURCE_STATE_COMMON;
  auto it = resource_states_.find(resource);
  return it != resource_states_.end() ? it->second
                                       : D3D12_RESOURCE_STATE_COMMON;
}

void CommandContext::UavBarrier(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* resource) {
  if (command_list == nullptr || resource == nullptr) return;
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.UAV.pResource = resource;
  command_list->ResourceBarrier(1, &barrier);
  TRACE_CMD(this, "uav_barrier res=%d(%s)", trace::ResourceId(resource),
            trace::ResourceNameOr(resource, "-"));
}

void CommandContext::CopyResourceToBackbuffer(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    ID3D12Resource* backbuffer) {
  if (command_list == nullptr || source == nullptr || backbuffer == nullptr) return;

  TRACE_CMD(this, "copy_to_backbuffer src=%d(%s) dst=%d(%s)",
            trace::ResourceId(source),
            trace::ResourceNameOr(source, "-"),
            trace::ResourceId(backbuffer),
            trace::ResourceNameOr(backbuffer, "-"));

  Transition(command_list, source, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(command_list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list->CopyResource(backbuffer, source);
  Transition(command_list, backbuffer, D3D12_RESOURCE_STATE_PRESENT);
  Transition(command_list, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void CommandContext::Shutdown() {
  if (fence_event_ != INVALID_HANDLE_VALUE) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }

  fence_.Reset();
  command_list_.Reset();
  command_allocator_.Reset();
  resource_states_.clear();

  fence_value_ = 0;
}

}  // namespace reshade_dlssnr

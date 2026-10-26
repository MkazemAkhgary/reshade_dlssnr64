#include "command_context.h"
#include "profiling/profile.h"

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

// Shutdown's drain normally returns at once: every caller has already proven
// the GPU idle, or is tearing down a device that will not progress again. The
// bound exists so the second case cannot hang the teardown forever.
constexpr DWORD kShutdownDrainTimeoutMs = 5000;

}  // namespace

bool CommandContext::Ensure(ID3D12Device* device) {
  DLSSNR_PROFILE_RESOURCE_L2("CommandContext::Ensure");
  if (frames_[0].allocator != nullptr) return true;
  if (device == nullptr) return false;

  for (FrameCommands& frame : frames_) {
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&frame.allocator)))) {
      Shutdown();
      return false;
    }

    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
            IID_PPV_ARGS(&frame.command_list)))) {
      Shutdown();
      return false;
    }

    const HRESULT close_hr = frame.command_list->Close();
    if (FAILED(close_hr)) {
      LogFailureOnce("initial command list close", close_hr);
      Shutdown();
      return false;
    }
  }

  if (FAILED(device->CreateFence(
          0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    Shutdown();
    return false;
  }

  fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) {
    Shutdown();
    return false;
  }

  current_frame_ = 0;
  fence_value_ = 0;
  last_signaled_fence_value_ = 0;
  recording_ = false;
  submitted_since_reset_ = false;
  return true;
}

bool CommandContext::Abort() {
  DLSSNR_PROFILE_ASYNC_L2("CommandContext::Abort");
  FrameCommands& frame = frames_[current_frame_];
  if (frame.command_list == nullptr || frame.allocator == nullptr) return true;

  // A skipped/failed Present stage can return before its submit tail. Close and
  // discard that recording, then safely reuse the same, never-submitted slot.
  if (recording_) {
    const HRESULT hr = frame.command_list->Close();
    if (FAILED(hr)) {
      LogFailureOnce("abandoned command list close", hr);
      return false;
    }
    recording_ = false;
  }

  return true;
}

bool CommandContext::WaitForFenceValue(UINT64 value, DWORD timeout_ms) {
  DLSSNR_PROFILE_ASYNC_L3("CommandContext::WaitForFenceValue");
  if (value == 0 || fence_ == nullptr || fence_->GetCompletedValue() >= value) {
    return true;
  }
  if (fence_event_ == INVALID_HANDLE_VALUE || fence_event_ == nullptr) {
    return false;
  }
  const HRESULT hr = fence_->SetEventOnCompletion(value, fence_event_);
  if (FAILED(hr)) {
    LogFailureOnce("fence SetEventOnCompletion", hr);
    return false;
  }
  return WaitForSingleObject(fence_event_, timeout_ms) == WAIT_OBJECT_0;
}

void CommandContext::Reset() {
  FrameCommands& previous = frames_[current_frame_];
  if (previous.command_list == nullptr || previous.allocator == nullptr) return;

  if (recording_) {
    if (!Abort()) return;
  } else if (submitted_since_reset_) {
    current_frame_ = (current_frame_ + 1) % kCommandRingSize;
  }

  FrameCommands& frame = frames_[current_frame_];
  // This wait happens only when the allocator/list slot is reused, up to once
  // every kCommandRingSize submissions; the common next-frame path is free to
  // record while earlier lists remain queued on the GPU.
  if (!WaitForFenceValue(frame.fence_value)) return;

  const HRESULT allocator_hr = frame.allocator->Reset();
  if (FAILED(allocator_hr)) {
    LogFailureOnce("command allocator reset", allocator_hr);
    return;
  }

  const HRESULT list_hr = frame.command_list->Reset(frame.allocator.Get(), nullptr);
  if (FAILED(list_hr)) {
    LogFailureOnce("command list reset", list_hr);
    return;
  }

  recording_ = true;
  submitted_since_reset_ = false;
}

void CommandContext::ClearState() {
  resource_states_.clear();
}

void CommandContext::CloseAndSubmit(ID3D12CommandQueue* command_queue) {
  DLSSNR_PROFILE_ASYNC_L2("CommandContext::CloseAndSubmit");
  FrameCommands& frame = frames_[current_frame_];
  if (frame.command_list == nullptr || command_queue == nullptr) return;

  const HRESULT close_hr = frame.command_list->Close();
  LogFailureOnce("command list close", close_hr);
  if (FAILED(close_hr)) return;
  recording_ = false;

  ID3D12CommandList* lists[] = {frame.command_list.Get()};
  command_queue->ExecuteCommandLists(1, lists);
  submitted_since_reset_ = true;
  TRACE_CMD(this, "close_and_submit queue=%d slot=%zu",
            trace::ResourceId(command_queue), current_frame_);

  ID3D12Device* device = nullptr;
  if (SUCCEEDED(frame.command_list->GetDevice(IID_PPV_ARGS(&device))) &&
      device != nullptr) {
    LogFailureOnce("device removed after execute_command_lists",
                   device->GetDeviceRemovedReason());
    device->Release();
  }
}

uint64_t CommandContext::SignalGpu(ID3D12CommandQueue* command_queue) {
  DLSSNR_PROFILE_ASYNC_L2("CommandContext::SignalGpu");
  if (command_queue == nullptr || fence_ == nullptr) return 0;

  const UINT64 value = fence_value_ + 1;
  const HRESULT hr = command_queue->Signal(fence_.Get(), value);
  if (FAILED(hr)) {
    LogFailureOnce("command queue fence signal", hr);
    return 0;
  }
  fence_value_ = value;
  last_signaled_fence_value_ = value;
  if (submitted_since_reset_) frames_[current_frame_].fence_value = value;
  TRACE_CMD(this, "signal_gpu value=%llu queue=%d slot=%zu",
            static_cast<unsigned long long>(value),
            trace::ResourceId(command_queue), current_frame_);
  return value;
}

void CommandContext::WaitGpu(ID3D12CommandQueue* command_queue) {
  DLSSNR_PROFILE_ASYNC_L3("CommandContext::WaitGpu");
  const UINT64 value = SignalGpu(command_queue);
  if (value == 0) return;
  WaitForFenceValue(value);
  TRACE_CMD(this, "wait_gpu done signal=%llu",
            static_cast<unsigned long long>(value));
}

uint64_t CommandContext::CompletedFenceValue() const {
  return fence_ != nullptr ? fence_->GetCompletedValue() : 0;
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

void CommandContext::TransitionBatch(
    ID3D12GraphicsCommandList* command_list,
    const ResourceTransition* transitions,
    size_t count) {
  if (command_list == nullptr || transitions == nullptr || count == 0) return;

  std::array<D3D12_RESOURCE_BARRIER, 16> barriers = {};
  UINT barrier_count = 0;
  auto flush = [&]() {
    if (barrier_count == 0) return;
    command_list->ResourceBarrier(barrier_count, barriers.data());
    barrier_count = 0;
  };

  for (size_t i = 0; i < count; ++i) {
    ID3D12Resource* resource = transitions[i].resource;
    if (resource == nullptr) continue;

    auto it = resource_states_.find(resource);
    const bool tracked = it != resource_states_.end();
    const D3D12_RESOURCE_STATES before =
        tracked ? it->second : InferResourceState(resource);
    const D3D12_RESOURCE_STATES after = transitions[i].after;

    if (before == after) {
      resource_states_[resource] = after;
      TRACE_CMD(this, "transition res=%d(%s) %s SKIP(same) tracked=%d",
                trace::ResourceId(resource),
                trace::ResourceNameOr(resource, "-"),
                trace::StateName(before), tracked ? 1 : 0);
      continue;
    }

    D3D12_RESOURCE_BARRIER& barrier = barriers[barrier_count++];
    barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;

    TRACE_CMD(this, "transition res=%d(%s) %s tracked=%d",
              trace::ResourceId(resource),
              trace::ResourceNameOr(resource, "-"),
              trace::StatePairName(before, after), tracked ? 1 : 0);
    resource_states_[resource] = after;

    if (barrier_count == static_cast<UINT>(barriers.size())) flush();
  }
  flush();
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
  DLSSNR_PROFILE_RESOURCE_L2("CommandContext::Shutdown");
  Abort();
  // Shutdown is rare; drain all submitted commands before releasing allocator
  // and list objects. The frame path uses rotating slots and never does this.
  if (fence_ != nullptr && last_signaled_fence_value_ != 0) {
    WaitForFenceValue(last_signaled_fence_value_, kShutdownDrainTimeoutMs);
  }
  for (FrameCommands& frame : frames_) {
    frame.command_list.Reset();
    frame.allocator.Reset();
    frame.fence_value = 0;
  }
  fence_.Reset();
  if (fence_event_ != INVALID_HANDLE_VALUE && fence_event_ != nullptr) {
    CloseHandle(fence_event_);
    fence_event_ = INVALID_HANDLE_VALUE;
  }
  current_frame_ = 0;
  fence_value_ = 0;
  last_signaled_fence_value_ = 0;
  recording_ = false;
  submitted_since_reset_ = false;
  resource_states_.clear();
}

}  // namespace reshade_dlssnr

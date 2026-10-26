#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>

#include "profiling/profile.h"

namespace reshade_dlssnr::profiling {

#if !DLSSNR_PROFILE_TRACY_ENABLED
// When the Tracy backend is compiled out, profile.h does not pull in the
// Tracy headers and TracyD3D12Ctx is not declared. Tracy's real type is
// an opaque pointer, so void* is interchangeable for our use.
using TracyD3D12Ctx = void*;
#endif

// Creates the Tracy D3D12 context for the device/queue pair. Safe to call
// repeatedly; only the first call has an effect.
void InitializeGpu(ID3D12Device* device, ID3D12CommandQueue* queue);

// Releases the D3D12 context. The context captures a device and a queue, so a
// device swap has to drop it before the next InitializeGpu builds a new one.
void ShutdownGpu();

// Marks a frame boundary and collects any GPU timestamps recorded during
// the frame. Call once per Present, after all GPU work has been submitted.
void MarkPresent(IDXGISwapChain* swapchain);

// The D3D12 context used by the GPU zone macros in profile.h. Null until
// InitializeGpu has run.
TracyD3D12Ctx GetD3D12Context();

// The command list recorded by the most recent DLSSNR_PROFILE_GPU_CONTEXT_L2.
// Thread-local because each command list is recorded from one thread.
void SetGpuCommandList(ID3D12GraphicsCommandList* command_list);
ID3D12GraphicsCommandList* GetGpuCommandList();

}  // namespace reshade_dlssnr::profiling

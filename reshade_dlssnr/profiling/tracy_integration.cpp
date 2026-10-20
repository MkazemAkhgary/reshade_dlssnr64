#include "tracy_integration.h"
#include "profiling/profile.h"

#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
#include <tracy/TracyD3D12.hpp>
#endif

#include <cstdint>

namespace reshade_dlssnr::profiling {

namespace {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
TracyD3D12Ctx g_d3d12_ctx = nullptr;
thread_local ID3D12GraphicsCommandList* g_gpu_command_list = nullptr;
#endif
}  // namespace

void InitializeGpu(ID3D12Device* device, ID3D12CommandQueue* queue) {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
  if (device != nullptr && queue != nullptr && g_d3d12_ctx == nullptr) {
    g_d3d12_ctx = TracyD3D12Context(device, queue);
  }
#else
  (void)device;
  (void)queue;
#endif
}

void MarkPresent(IDXGISwapChain* swapchain) {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
  if (g_d3d12_ctx != nullptr) {
    TracyD3D12NewFrame(g_d3d12_ctx);
    TracyD3D12Collect(g_d3d12_ctx);
  }
#else
  (void)swapchain;
#endif
}

TracyD3D12Ctx GetD3D12Context() {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
  return g_d3d12_ctx;
#else
  return nullptr;
#endif
}

void SetGpuCommandList(ID3D12GraphicsCommandList* command_list) {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
  g_gpu_command_list = command_list;
#else
  (void)command_list;
#endif
}

ID3D12GraphicsCommandList* GetGpuCommandList() {
#if DLSSNR_PROFILE_TRACY_ENABLED && DLSSNR_PROFILE_VERBOSITY >= 2 && DLSSNR_PROFILE_DOMAIN_GPU
  return g_gpu_command_list;
#else
  return nullptr;
#endif
}

}  // namespace reshade_dlssnr::profiling

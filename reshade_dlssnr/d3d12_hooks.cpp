#include "d3d12_hooks.h"

#include <detours.h>

#include <reshade.hpp>

#include "d3d12_state.h"

namespace reshade_dlssnr {

namespace {

using PFN_SetDescriptorHeaps = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using PFN_SetPipelineState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetComputeRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetGraphicsRootSignature = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);

PFN_SetDescriptorHeaps g_original_set_descriptor_heaps = nullptr;
PFN_SetPipelineState g_original_set_pipeline_state = nullptr;
PFN_SetComputeRootSignature g_original_set_compute_root_signature = nullptr;
PFN_SetGraphicsRootSignature g_original_set_graphics_root_signature = nullptr;

bool g_hooked_command_lists = false;

void STDMETHODCALLTYPE HookedSetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  if (command_list != nullptr) {
    D3D12StateTracker::Get().OnSetDescriptorHeaps(command_list, count, heaps);
  }
  g_original_set_descriptor_heaps(command_list, count, heaps);
}

void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
  if (command_list != nullptr) {
    D3D12StateTracker::Get().OnSetPipelineState(command_list, pipeline_state);
  }
  g_original_set_pipeline_state(command_list, pipeline_state);
}

void STDMETHODCALLTYPE HookedSetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list != nullptr) {
    D3D12StateTracker::Get().OnSetComputeRootSignature(command_list, root_signature);
  }
  g_original_set_compute_root_signature(command_list, root_signature);
}

void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list != nullptr) {
    D3D12StateTracker::Get().OnSetGraphicsRootSignature(command_list, root_signature);
  }
  g_original_set_graphics_root_signature(command_list, root_signature);
}

}  // namespace

D3D12Hooks& D3D12Hooks::Get() {
  static D3D12Hooks hooks;
  return hooks;
}

void D3D12Hooks::HookCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  void** vtable = *reinterpret_cast<void***>(command_list);
  if (vtable == nullptr) return;

  D3D12StateTracker::Get().TrackCommandList(command_list);

  if (g_hooked_command_lists) {
    return;
  }

  g_original_set_pipeline_state = reinterpret_cast<PFN_SetPipelineState>(vtable[25]);
  g_original_set_descriptor_heaps = reinterpret_cast<PFN_SetDescriptorHeaps>(vtable[28]);
  g_original_set_compute_root_signature = reinterpret_cast<PFN_SetComputeRootSignature>(vtable[29]);
  g_original_set_graphics_root_signature = reinterpret_cast<PFN_SetGraphicsRootSignature>(vtable[30]);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  DetourAttach(reinterpret_cast<void**>(&g_original_set_pipeline_state), HookedSetPipelineState);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_descriptor_heaps), HookedSetDescriptorHeaps);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_signature), HookedSetComputeRootSignature);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_graphics_root_signature), HookedSetGraphicsRootSignature);

  if (DetourTransactionCommit() == NO_ERROR) {
    g_hooked_command_lists = true;
  }
}

void D3D12Hooks::UnhookCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  D3D12StateTracker::Get().UntrackCommandList(command_list);
}

}  // namespace reshade_dlssnr

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
using PFN_SetComputeRootDescriptorTable = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetComputeRoot32BitConstant = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_SetComputeRoot32BitConstants = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_SetComputeRootConstantBufferView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_SetComputeRootShaderResourceView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_SetComputeRootUnorderedAccessView = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);

struct RootRestoreHook {
  void* original = nullptr;
};

PFN_SetDescriptorHeaps g_original_set_descriptor_heaps = nullptr;
PFN_SetPipelineState g_original_set_pipeline_state = nullptr;
PFN_SetComputeRootSignature g_original_set_compute_root_signature = nullptr;
PFN_SetGraphicsRootSignature g_original_set_graphics_root_signature = nullptr;
PFN_SetComputeRootDescriptorTable g_original_set_compute_root_descriptor_table = nullptr;
PFN_SetComputeRoot32BitConstant g_original_set_compute_root_32bit_constant = nullptr;
PFN_SetComputeRoot32BitConstants g_original_set_compute_root_32bit_constants = nullptr;
PFN_SetComputeRootConstantBufferView g_original_set_compute_root_cbv = nullptr;
PFN_SetComputeRootShaderResourceView g_original_set_compute_root_srv = nullptr;
PFN_SetComputeRootUnorderedAccessView g_original_set_compute_root_uav = nullptr;

bool g_hooked_command_lists = false;

thread_local bool g_in_set_descriptor_heaps = false;
thread_local bool g_in_set_pipeline_state = false;
thread_local bool g_in_set_compute_root_signature = false;
thread_local bool g_in_set_graphics_root_signature = false;
thread_local bool g_in_set_compute_root_descriptor_table = false;
thread_local bool g_in_set_compute_root_32bit_constant = false;
thread_local bool g_in_set_compute_root_32bit_constants = false;
thread_local bool g_in_set_compute_root_cbv = false;
thread_local bool g_in_set_compute_root_srv = false;
thread_local bool g_in_set_compute_root_uav = false;

void STDMETHODCALLTYPE HookedSetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  if (!g_in_set_descriptor_heaps && command_list != nullptr) {
    D3D12StateTracker::Get().SetDescriptorHeaps(command_list, count, heaps);
  }
  g_original_set_descriptor_heaps(command_list, count, heaps);
}

void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
  if (!g_in_set_pipeline_state && command_list != nullptr) {
    D3D12StateTracker::Get().SetPipelineState(command_list, pipeline_state);
  }
  g_original_set_pipeline_state(command_list, pipeline_state);
}

void STDMETHODCALLTYPE HookedSetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (!g_in_set_compute_root_signature && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootSignature(command_list, root_signature);
  }
  g_original_set_compute_root_signature(command_list, root_signature);
}

void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (!g_in_set_graphics_root_signature && command_list != nullptr) {
    D3D12StateTracker::Get().SetGraphicsRootSignature(command_list, root_signature);
  }
  g_original_set_graphics_root_signature(command_list, root_signature);
}

void STDMETHODCALLTYPE HookedSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  if (!g_in_set_compute_root_descriptor_table && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootDescriptorTable(command_list, index, base_descriptor);
  }
  g_original_set_compute_root_descriptor_table(command_list, index, base_descriptor);
}

void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstant(ID3D12GraphicsCommandList* command_list, UINT index, UINT value, UINT offset) {
  if (!g_in_set_compute_root_32bit_constant && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRoot32BitConstant(command_list, index, value, offset);
  }
  g_original_set_compute_root_32bit_constant(command_list, index, value, offset);
}

void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* command_list, UINT index, UINT count, const void* data, UINT offset) {
  if (!g_in_set_compute_root_32bit_constants && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRoot32BitConstants(command_list, index, count, data, offset);
  }
  g_original_set_compute_root_32bit_constants(command_list, index, count, data, offset);
}

void STDMETHODCALLTYPE HookedSetComputeRootConstantBufferView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!g_in_set_compute_root_cbv && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootConstantBufferView(command_list, index, address);
  }
  g_original_set_compute_root_cbv(command_list, index, address);
}

void STDMETHODCALLTYPE HookedSetComputeRootShaderResourceView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!g_in_set_compute_root_srv && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootShaderResourceView(command_list, index, address);
  }
  g_original_set_compute_root_srv(command_list, index, address);
}

void STDMETHODCALLTYPE HookedSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!g_in_set_compute_root_uav && command_list != nullptr) {
    D3D12StateTracker::Get().SetComputeRootUnorderedAccessView(command_list, index, address);
  }
  g_original_set_compute_root_uav(command_list, index, address);
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
  g_original_set_compute_root_descriptor_table = reinterpret_cast<PFN_SetComputeRootDescriptorTable>(vtable[31]);
  g_original_set_compute_root_32bit_constant = reinterpret_cast<PFN_SetComputeRoot32BitConstant>(vtable[33]);
  g_original_set_compute_root_32bit_constants = reinterpret_cast<PFN_SetComputeRoot32BitConstants>(vtable[35]);
  g_original_set_compute_root_cbv = reinterpret_cast<PFN_SetComputeRootConstantBufferView>(vtable[37]);
  g_original_set_compute_root_srv = reinterpret_cast<PFN_SetComputeRootShaderResourceView>(vtable[39]);
  g_original_set_compute_root_uav = reinterpret_cast<PFN_SetComputeRootUnorderedAccessView>(vtable[41]);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());

  DetourAttach(reinterpret_cast<void**>(&g_original_set_pipeline_state), HookedSetPipelineState);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_descriptor_heaps), HookedSetDescriptorHeaps);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_signature), HookedSetComputeRootSignature);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_graphics_root_signature), HookedSetGraphicsRootSignature);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_descriptor_table), HookedSetComputeRootDescriptorTable);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_32bit_constant), HookedSetComputeRoot32BitConstant);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_32bit_constants), HookedSetComputeRoot32BitConstants);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_cbv), HookedSetComputeRootConstantBufferView);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_srv), HookedSetComputeRootShaderResourceView);
  DetourAttach(reinterpret_cast<void**>(&g_original_set_compute_root_uav), HookedSetComputeRootUnorderedAccessView);

  if (DetourTransactionCommit() == NO_ERROR) {
    g_hooked_command_lists = true;
  }
}

void D3D12Hooks::UnhookCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  D3D12StateTracker::Get().UntrackCommandList(command_list);
}

void D3D12Hooks::RestoreCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  const auto heaps = D3D12StateTracker::Get().GetDescriptorHeaps(command_list);
  if (heaps.NumDescriptorHeaps > 0 && heaps.Heaps[0] != nullptr) {
    g_original_set_descriptor_heaps(command_list, heaps.NumDescriptorHeaps, heaps.Heaps);
  }

  const auto signature = D3D12StateTracker::Get().GetSignature(command_list);
  if (signature.type == SignatureEntryType::Compute && signature.ptr != nullptr) {
    g_original_set_compute_root_signature(command_list, signature.ptr);
  } else if (signature.type == SignatureEntryType::Graphics && signature.ptr != nullptr) {
    g_original_set_graphics_root_signature(command_list, signature.ptr);
  }

  const auto* root_state = D3D12StateTracker::Get().GetRootState(command_list);
  if (root_state != nullptr) {
    for (UINT i = 0; i < root_state->size(); ++i) {
      const RootState& state = (*root_state)[i];
      switch (state.type) {
        case RootEntryType::Table:
          g_original_set_compute_root_descriptor_table(command_list, i, state.rootDescriptorTable);
          break;
        case RootEntryType::Constant:
          g_original_set_compute_root_32bit_constant(command_list, i, state.Data[0], state.DestOffset);
          break;
        case RootEntryType::Constants:
          g_original_set_compute_root_32bit_constants(command_list, i, state.Num32BitValues, state.Data.data(), state.DestOffset);
          break;
        case RootEntryType::CBV:
          g_original_set_compute_root_cbv(command_list, i, state.bufferLocation);
          break;
        case RootEntryType::SRV:
          g_original_set_compute_root_srv(command_list, i, state.bufferLocation);
          break;
        case RootEntryType::UAV:
          g_original_set_compute_root_uav(command_list, i, state.bufferLocation);
          break;
        default:
          break;
      }
    }
  }

  ID3D12PipelineState* pipeline_state = D3D12StateTracker::Get().GetPipelineState(command_list);
  if (pipeline_state != nullptr) {
    g_original_set_pipeline_state(command_list, pipeline_state);
  }
}

}  // namespace reshade_dlssnr

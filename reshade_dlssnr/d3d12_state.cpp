#include "d3d12_state.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

D3D12StateTracker& D3D12StateTracker::Get() {
  static D3D12StateTracker tracker;
  return tracker;
}

void D3D12StateTracker::TrackCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  states_.try_emplace(command_list);
}

void D3D12StateTracker::UntrackCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;
  states_.erase(command_list);
}

void D3D12StateTracker::OnSetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  if (command_list == nullptr || heaps == nullptr || count == 0) return;
  auto it = states_.find(command_list);
  if (it == states_.end()) return;

  it->second.descriptor_heap_count = count;
  for (UINT i = 0; i < count && i < 2; ++i) {
    it->second.descriptor_heaps[i] = heaps[i];
  }
}

void D3D12StateTracker::OnSetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
  if (command_list == nullptr) return;
  states_[command_list].pipeline_state = pipeline_state;
}

void D3D12StateTracker::OnSetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list == nullptr) return;
  states_[command_list].compute_root_signature = root_signature;
}

void D3D12StateTracker::OnSetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list == nullptr) return;
  states_[command_list].graphics_root_signature = root_signature;
}

void D3D12StateTracker::OnSetComputeRootDescriptorTable(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE) {}
void D3D12StateTracker::OnSetComputeRoot32BitConstants(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT) {}
void D3D12StateTracker::OnSetComputeRootConstantBufferView(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}
void D3D12StateTracker::OnSetComputeRootShaderResourceView(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}
void D3D12StateTracker::OnSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS) {}

bool D3D12StateTracker::SaveAndInjectComputeState(
    ID3D12GraphicsCommandList* command_list,
    ID3D12RootSignature* root_signature,
    ID3D12PipelineState* pipeline_state,
    ID3D12DescriptorHeap* const* heaps,
    UINT heap_count) {
  if (command_list == nullptr) return false;
  states_.try_emplace(command_list);

  command_list->SetDescriptorHeaps(heap_count, heaps);
  command_list->SetComputeRootSignature(root_signature);
  command_list->SetPipelineState(pipeline_state);
  return true;
}

void D3D12StateTracker::RestoreState(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  auto it = states_.find(command_list);
  if (it == states_.end()) return;

  const State& state = it->second;

  if (state.descriptor_heap_count > 0 && state.descriptor_heaps[0] != nullptr) {
    command_list->SetDescriptorHeaps(
        state.descriptor_heap_count,
        state.descriptor_heaps);
  }

  if (state.compute_root_signature != nullptr) {
    command_list->SetComputeRootSignature(state.compute_root_signature);
  }

  if (state.graphics_root_signature != nullptr) {
    command_list->SetGraphicsRootSignature(state.graphics_root_signature);
  }

  if (state.pipeline_state != nullptr) {
    command_list->SetPipelineState(state.pipeline_state);
  }
}

}  // namespace reshade_dlssnr

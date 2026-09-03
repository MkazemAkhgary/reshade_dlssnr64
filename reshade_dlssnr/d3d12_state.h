#pragma once

#include <d3d12.h>
#include <windows.h>

#include <unordered_map>

namespace reshade_dlssnr {

class D3D12StateTracker {
 public:
  static D3D12StateTracker& Get();

  void TrackCommandList(ID3D12GraphicsCommandList* command_list);
  void UntrackCommandList(ID3D12GraphicsCommandList* command_list);

  void OnSetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps);
  void OnSetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state);
  void OnSetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature);
  void OnSetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature);
  void OnSetComputeRootDescriptorTable(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);
  void OnSetComputeRoot32BitConstants(ID3D12GraphicsCommandList* command_list, UINT index, UINT count, const void* data, UINT offset);
  void OnSetComputeRootConstantBufferView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
  void OnSetComputeRootShaderResourceView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
  void OnSetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);

  bool SaveAndInjectComputeState(
      ID3D12GraphicsCommandList* command_list,
      ID3D12RootSignature* root_signature,
      ID3D12PipelineState* pipeline_state,
      ID3D12DescriptorHeap* const* heaps,
      UINT heap_count);

  void RestoreState(ID3D12GraphicsCommandList* command_list);

 private:
  D3D12StateTracker() = default;
  ~D3D12StateTracker() = default;

  D3D12StateTracker(const D3D12StateTracker&) = delete;
  D3D12StateTracker& operator=(const D3D12StateTracker&) = delete;

  struct State {
    ID3D12PipelineState* pipeline_state = nullptr;
    ID3D12RootSignature* compute_root_signature = nullptr;
    ID3D12RootSignature* graphics_root_signature = nullptr;
    UINT descriptor_heap_count = 0;
    ID3D12DescriptorHeap* descriptor_heaps[2] = {nullptr, nullptr};
  };

  std::unordered_map<ID3D12GraphicsCommandList*, State> states_;
};

}  // namespace reshade_dlssnr

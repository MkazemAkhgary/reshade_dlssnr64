// Tracks D3D12 command-list state for safe restore around injected work.
#pragma once

#include <d3d12.h>
#include <windows.h>

#include <shared_mutex>
#include <vector>

#include <unordered_map>

namespace reshade_dlssnr {

enum class RootEntryType {
  Invalid,
  Table,
  Constant,
  Constants,
  CBV,
  SRV,
  UAV,
};

struct RootState {
  RootEntryType type = RootEntryType::Invalid;
  D3D12_GPU_DESCRIPTOR_HANDLE rootDescriptorTable {};
  D3D12_GPU_VIRTUAL_ADDRESS bufferLocation {};
  UINT Num32BitValues = 0;
  UINT DestOffset = 0;
  std::vector<uint32_t> Data;
};

// D3D12 keeps a graphics and a compute root signature bound at the same time,
// and each has its own root parameters: SetComputeRoot* and SetGraphicsRoot*
// address the same index numbers in two different signatures. The tracker only
// observes the compute calls, so only that slot has root entries -- but the two
// have to be kept apart, because replaying compute indices against a graphics
// signature addresses parameters it does not have, and the driver faults.
struct SignatureState {
  ID3D12RootSignature* compute = nullptr;
  ID3D12RootSignature* graphics = nullptr;
};

struct DescriptorHeapState {
  UINT NumDescriptorHeaps = 0;
  ID3D12DescriptorHeap* Heaps[2] = {nullptr, nullptr};
};

class D3D12StateTracker {
 public:
  static D3D12StateTracker& Get();

  void TrackCommandList(ID3D12GraphicsCommandList* command_list);
  void UntrackCommandList(ID3D12GraphicsCommandList* command_list);

  // Drops every tracked list and releases the references it holds. A device
  // swap leaves them naming objects from the old device, and holding those
  // references keeps that device alive.
  void Clear();

  void SetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps);
  void SetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state);
  // Ray tracing pipeline state. Streamline's manual-hooking guide lists it
  // alongside SetPipelineState as state the host has to put back after an
  // evaluate, and it lives on ID3D12GraphicsCommandList4 rather than on the
  // base interface.
  void SetPipelineState1(ID3D12GraphicsCommandList* command_list, ID3D12StateObject* state_object);
  void SetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature);
  void SetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature);

  void SetComputeRootDescriptorTable(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);
  void SetComputeRoot32BitConstants(ID3D12GraphicsCommandList* command_list, UINT index, UINT count, const void* data, UINT offset);
  void SetComputeRoot32BitConstant(ID3D12GraphicsCommandList* command_list, UINT index, UINT value, UINT offset);
  void SetComputeRootConstantBufferView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
  void SetComputeRootShaderResourceView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);
  void SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address);

  bool HasState(ID3D12GraphicsCommandList* command_list) const;
  bool HasDescriptorHeaps(ID3D12GraphicsCommandList* command_list) const;
  bool HasPipelineState(ID3D12GraphicsCommandList* command_list) const;
  bool HasRootSignature(ID3D12GraphicsCommandList* command_list) const;
  bool HasRootState(ID3D12GraphicsCommandList* command_list) const;

  DescriptorHeapState GetDescriptorHeaps(ID3D12GraphicsCommandList* command_list) const;
  ID3D12PipelineState* GetPipelineState(ID3D12GraphicsCommandList* command_list) const;
  ID3D12StateObject* GetPipelineState1(ID3D12GraphicsCommandList* command_list) const;
  SignatureState GetSignatures(ID3D12GraphicsCommandList* command_list) const;
  const std::vector<RootState>* GetRootState(ID3D12GraphicsCommandList* command_list) const;

  void SetRootParameterCount(ID3D12RootSignature* root_signature, UINT count);
  UINT GetRootParameterCount(ID3D12RootSignature* root_signature) const;

 private:
  D3D12StateTracker() = default;
  ~D3D12StateTracker() = default;

  // Root slot `index`, growing the table when the root signature's parameter
  // count is still unknown. Caller must hold root_states_mutex_.
  RootState& RootSlotLocked(ID3D12GraphicsCommandList* command_list, UINT index);

  D3D12StateTracker(const D3D12StateTracker&) = delete;
  D3D12StateTracker& operator=(const D3D12StateTracker&) = delete;

  mutable std::shared_mutex descriptor_heaps_mutex_;
  std::unordered_map<ID3D12GraphicsCommandList*, DescriptorHeapState> descriptor_heaps_;

  mutable std::shared_mutex pipeline_states_mutex_;
  std::unordered_map<ID3D12GraphicsCommandList*, ID3D12PipelineState*> pipeline_states_;

  mutable std::shared_mutex state_objects_mutex_;
  std::unordered_map<ID3D12GraphicsCommandList*, ID3D12StateObject*> state_objects_;

  mutable std::shared_mutex root_signature_mutex_;
  std::unordered_map<ID3D12GraphicsCommandList*, SignatureState> signatures_;

  mutable std::shared_mutex root_states_mutex_;
  std::unordered_map<ID3D12GraphicsCommandList*, std::vector<RootState>> root_states_;

  // Which root signature the recorded root entries were made under. Binding a
  // different one invalidates them: index 3 of one signature is not index 3
  // of the next, and replaying the old entries against the new signature
  // writes root parameters the shader never declared.
  std::unordered_map<ID3D12GraphicsCommandList*, ID3D12RootSignature*> root_state_owner_;

  mutable std::shared_mutex root_parameter_count_mutex_;
  std::unordered_map<ID3D12RootSignature*, UINT> root_parameter_count_;
};

}  // namespace reshade_dlssnr

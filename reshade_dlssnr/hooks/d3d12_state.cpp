#include "d3d12_state.h"

namespace reshade_dlssnr {

D3D12StateTracker& D3D12StateTracker::Get() {
  static D3D12StateTracker tracker;
  return tracker;
}

void D3D12StateTracker::TrackCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  descriptor_heaps_.try_emplace(command_list);
  pipeline_states_.try_emplace(command_list);
  signatures_.try_emplace(command_list);
  root_states_.try_emplace(command_list);
}

void D3D12StateTracker::UntrackCommandList(ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr) return;

  descriptor_heaps_.erase(command_list);
  pipeline_states_.erase(command_list);
  state_objects_.erase(command_list);
  signatures_.erase(command_list);
  root_states_.erase(command_list);
  root_state_owner_.erase(command_list);
}

void D3D12StateTracker::SetDescriptorHeaps(ID3D12GraphicsCommandList* command_list, UINT count, ID3D12DescriptorHeap* const* heaps) {
  if (command_list == nullptr || heaps == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(descriptor_heaps_mutex_);
  DescriptorHeapState state {};
  state.NumDescriptorHeaps = count;
  for (UINT i = 0; i < count && i < 2; ++i) {
    state.Heaps[i] = heaps[i];
  }
  descriptor_heaps_.insert_or_assign(command_list, state);
}

void D3D12StateTracker::SetPipelineState(ID3D12GraphicsCommandList* command_list, ID3D12PipelineState* pipeline_state) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(pipeline_states_mutex_);
  pipeline_states_.insert_or_assign(command_list, pipeline_state);
}

RootState& D3D12StateTracker::RootSlotLocked(ID3D12GraphicsCommandList* command_list, UINT index) {
  auto& table = root_states_[command_list];
  // The parameter count is only known for root signatures we saw being
  // created, so a table sized from an unknown count starts empty and is grown
  // here. Dropping the write instead would leave the game's root state
  // unrestored after our passes.
  if (index >= table.size()) {
    table.resize(static_cast<size_t>(index) + 1);
  }
  return table[index];
}

void D3D12StateTracker::SetPipelineState1(ID3D12GraphicsCommandList* command_list, ID3D12StateObject* state_object) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(state_objects_mutex_);
  state_objects_.insert_or_assign(command_list, state_object);
}

void D3D12StateTracker::SetComputeRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list == nullptr || root_signature == nullptr) return;

  {
    std::unique_lock<std::shared_mutex> lock(root_parameter_count_mutex_);
    UINT count = root_parameter_count_[root_signature];
    std::unique_lock<std::shared_mutex> root_lock(root_states_mutex_);

    auto owner = root_state_owner_.find(command_list);
    auto& table = root_states_[command_list];
    if (owner == root_state_owner_.end() || owner->second != root_signature) {
      // A different signature is being bound. The recorded entries describe
      // the previous one's layout, and replaying them would write root
      // parameters this signature may not even have.
      table.assign(count, RootState{});
      root_state_owner_[command_list] = root_signature;
    } else if (table.size() < count) {
      table.resize(count);
    }
  }

  std::unique_lock<std::shared_mutex> lock(root_signature_mutex_);
  signatures_.insert_or_assign(command_list, SignatureEntry { SignatureEntryType::Compute, root_signature });
}

void D3D12StateTracker::SetGraphicsRootSignature(ID3D12GraphicsCommandList* command_list, ID3D12RootSignature* root_signature) {
  if (command_list == nullptr || root_signature == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_signature_mutex_);
  signatures_.insert_or_assign(command_list, SignatureEntry { SignatureEntryType::Graphics, root_signature });
}

void D3D12StateTracker::SetComputeRootDescriptorTable(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  if (command_list == nullptr || base_descriptor.ptr == 0) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::Table;
  slot.rootDescriptorTable = base_descriptor;
}

void D3D12StateTracker::SetComputeRoot32BitConstants(ID3D12GraphicsCommandList* command_list, UINT index, UINT count, const void* data, UINT offset) {
  if (command_list == nullptr || data == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::Constants;
  slot.Num32BitValues = count;
  slot.DestOffset = offset;
  const auto* src = static_cast<const uint32_t*>(data);
  slot.Data.assign(src, src + count);
}

void D3D12StateTracker::SetComputeRoot32BitConstant(ID3D12GraphicsCommandList* command_list, UINT index, UINT value, UINT offset) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::Constant;
  slot.DestOffset = offset;
  slot.Data.assign(1, value);
}

void D3D12StateTracker::SetComputeRootConstantBufferView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::CBV;
  slot.bufferLocation = address;
}

void D3D12StateTracker::SetComputeRootShaderResourceView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::SRV;
  slot.bufferLocation = address;
}

void D3D12StateTracker::SetComputeRootUnorderedAccessView(ID3D12GraphicsCommandList* command_list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (command_list == nullptr) return;

  std::unique_lock<std::shared_mutex> lock(root_states_mutex_);
  RootState& slot = RootSlotLocked(command_list, index);
  slot.type = RootEntryType::UAV;
  slot.bufferLocation = address;
}

bool D3D12StateTracker::HasState(ID3D12GraphicsCommandList* command_list) const {
  return HasDescriptorHeaps(command_list) || HasPipelineState(command_list) || HasRootSignature(command_list) || HasRootState(command_list);
}

bool D3D12StateTracker::HasDescriptorHeaps(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(descriptor_heaps_mutex_);
  return descriptor_heaps_.contains(command_list);
}

bool D3D12StateTracker::HasPipelineState(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(pipeline_states_mutex_);
  return pipeline_states_.contains(command_list);
}

bool D3D12StateTracker::HasRootSignature(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(root_signature_mutex_);
  return signatures_.contains(command_list);
}

bool D3D12StateTracker::HasRootState(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(root_states_mutex_);
  return root_states_.contains(command_list);
}

DescriptorHeapState D3D12StateTracker::GetDescriptorHeaps(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(descriptor_heaps_mutex_);
  auto it = descriptor_heaps_.find(command_list);
  if (it == descriptor_heaps_.end()) return {};
  return it->second;
}

ID3D12PipelineState* D3D12StateTracker::GetPipelineState(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(pipeline_states_mutex_);
  auto it = pipeline_states_.find(command_list);
  if (it == pipeline_states_.end()) return nullptr;
  return it->second;
}

ID3D12StateObject* D3D12StateTracker::GetPipelineState1(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(state_objects_mutex_);
  auto it = state_objects_.find(command_list);
  if (it == state_objects_.end()) return nullptr;
  return it->second;
}

SignatureEntry D3D12StateTracker::GetSignature(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(root_signature_mutex_);
  auto it = signatures_.find(command_list);
  if (it == signatures_.end()) return {};
  return it->second;
}

const std::vector<RootState>* D3D12StateTracker::GetRootState(ID3D12GraphicsCommandList* command_list) const {
  std::shared_lock<std::shared_mutex> lock(root_states_mutex_);
  auto it = root_states_.find(command_list);
  if (it == root_states_.end()) return nullptr;
  return &it->second;
}

void D3D12StateTracker::SetRootParameterCount(ID3D12RootSignature* root_signature, UINT count) {
  if (root_signature == nullptr) return;
  std::unique_lock<std::shared_mutex> lock(root_parameter_count_mutex_);
  root_parameter_count_.insert_or_assign(root_signature, count);
}

UINT D3D12StateTracker::GetRootParameterCount(ID3D12RootSignature* root_signature) const {
  std::shared_lock<std::shared_mutex> lock(root_parameter_count_mutex_);
  auto it = root_parameter_count_.find(root_signature);
  return it != root_parameter_count_.end() ? it->second : 0;
}

}  // namespace reshade_dlssnr

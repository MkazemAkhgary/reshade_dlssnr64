#include "codec.h"

#include <reshade.hpp>

#include "compute_passes.h"

namespace reshade_dlssnr {

DXGI_FORMAT ComputeEngine::TypedFormat(DXGI_FORMAT format) const {
  switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
      return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
      return format;
  }
}

void ComputeEngine::CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Format = TypedFormat(resource->GetDesc().Format);
  desc.Texture2D.MipLevels = 1;
  desc.Texture2D.MostDetailedMip = 0;
  desc.Texture2D.PlaneSlice = 0;
  desc.Texture2D.ResourceMinLODClamp = 0.0f;
  device_->CreateShaderResourceView(resource, &desc, handle);
}

void ComputeEngine::CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  desc.Format = TypedFormat(resource->GetDesc().Format);
  desc.Texture2D.MipSlice = 0;
  desc.Texture2D.PlaneSlice = 0;
  device_->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void ComputeEngine::CreateConstantBufferView(ID3D12Resource* resource, const void* data, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  void* mapped = nullptr;
  D3D12_RANGE read_range = {};
  if (SUCCEEDED(resource->Map(0, &read_range, &mapped))) {
    memcpy(mapped, data, size);
    D3D12_RANGE write_range = {0, size};
    resource->Unmap(0, &write_range);
  }

  D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
  desc.BufferLocation = resource->GetGPUVirtualAddress();
  desc.SizeInBytes = 256;
  device_->CreateConstantBufferView(&desc, handle);
}

bool ComputeEngine::BindAndDispatch(
    ID3D12GraphicsCommandList* command_list,
    ID3D12PipelineState* pipeline_state,
    const ComputeConstants& constants,
    ID3D12Resource* const* srvs,
    ID3D12Resource* const* uavs) {
  if (command_list == nullptr || pipeline_state == nullptr ||
      srvs == nullptr || uavs == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDescriptorRingSize;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) *
                    kDescriptorsPerDispatch * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
      descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) *
                    kDescriptorsPerDispatch * descriptor_size_;

  for (uint32_t i = 0; i < kSrvCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(i) * descriptor_size_;
    CreateShaderResourceView(srvs[i], handle);
  }

  for (uint32_t i = 0; i < kUavCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(kSrvCount + i) *
                  descriptor_size_;
    CreateUnorderedAccessView(uavs[i], handle);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = cpu_handle;
  cbv_handle.ptr += static_cast<SIZE_T>(kSrvCount + kUavCount) *
                    descriptor_size_;
  CreateConstantBufferView(
      constant_buffers_[slot].Get(), &constants, sizeof(constants), cbv_handle);

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_.Get()};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_.Get());
  command_list->SetPipelineState(pipeline_state);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(
      (constants.Width + 7) / 8,
      (constants.Height + 7) / 8,
      1);

  return true;
}

bool ComputeEngine::CreateRootSignature(ID3D12Device* device) {
  D3D12_DESCRIPTOR_RANGE ranges[3] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = kSrvCount;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kUavCount;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  ranges[2].NumDescriptors = kCbvCount;
  ranges[2].BaseShaderRegister = 0;
  ranges[2].RegisterSpace = 0;
  ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  D3D12_ROOT_PARAMETER root_params[1] = {};
  root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  root_params[0].DescriptorTable.NumDescriptorRanges = 3;
  root_params[0].DescriptorTable.pDescriptorRanges = ranges;

  D3D12_STATIC_SAMPLER_DESC static_sampler = {};
  static_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  static_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  static_sampler.MipLODBias = 0.0f;
  static_sampler.MaxAnisotropy = 0;
  static_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  static_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
  static_sampler.MinLOD = 0.0f;
  static_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  static_sampler.ShaderRegister = 0;
  static_sampler.RegisterSpace = 0;
  static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

  D3D12_ROOT_SIGNATURE_DESC root_desc = {};
  root_desc.NumParameters = 1;
  root_desc.pParameters = root_params;
  root_desc.NumStaticSamplers = 1;
  root_desc.pStaticSamplers = &static_sampler;
  root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ID3DBlob* signature = nullptr;
  ID3DBlob* error = nullptr;
  if (FAILED(D3D12SerializeRootSignature(
          &root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
    if (error != nullptr) {
      reshade::log::message(
          reshade::log::level::error,
          "DLSSNR codec root signature serialization failed");
      error->Release();
    }
    return false;
  }

  if (FAILED(device->CreateRootSignature(
          0,
          signature->GetBufferPointer(),
          signature->GetBufferSize(),
          IID_PPV_ARGS(root_signature_.GetAddressOf())))) {
    signature->Release();
    return false;
  }
  signature->Release();

  return true;
}

bool ComputeEngine::CompilePipeline(
    const char* hlsl,
    ID3D12PipelineState** out_pso) {
  if (device_ == nullptr || hlsl == nullptr || out_pso == nullptr) {
    return false;
  }

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          hlsl,
          strlen(hlsl),
          nullptr,
          nullptr,
          nullptr,
          "CSMain",
          "cs_5_1",
          0,
          0,
          &shader_blob,
          &error_blob))) {
    if (error_blob != nullptr) {
      reshade::log::message(
          reshade::log::level::error,
          static_cast<const char*>(error_blob->GetBufferPointer()));
      error_blob->Release();
    } else {
      reshade::log::message(
          reshade::log::level::error,
          "D3DCompile failed without an error blob");
    }
    return false;
  }

  if (error_blob != nullptr) error_blob->Release();

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
  pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
  pso_desc.NodeMask = 0;

  const HRESULT hr = device_->CreateComputePipelineState(
      &pso_desc, IID_PPV_ARGS(out_pso));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool ComputeEngine::Dispatch(
    const char* pass_name,
    ID3D12GraphicsCommandList* command_list,
    const ComputeConstants& constants,
    ID3D12Resource* const* srvs,
    ID3D12Resource* const* uavs) {
  if (device_ == nullptr || pass_name == nullptr ||
      command_list == nullptr || srvs == nullptr || uavs == nullptr) {
    return false;
  }

  auto it = passes_.find(pass_name);
  if (it == passes_.end()) {
    const char* source = FindComputePassSource(pass_name);
    if (source == nullptr) return false;

    CachedPass pass;
    if (!CompilePipeline(source, pass.pso.GetAddressOf())) {
      std::string msg = "Failed to compile compute pass: ";
      msg += pass_name;
      reshade::log::message(reshade::log::level::error, msg.c_str());
      return false;
    }

    it = passes_.emplace(pass_name, std::move(pass)).first;
  }

  return BindAndDispatch(
      command_list, it->second.pso.Get(), constants, srvs, uavs);
}

bool ComputeEngine::Init(ID3D12Device* device) {
  if (device_ != nullptr) return true;
  if (device == nullptr) return false;

  if (!CreateRootSignature(device)) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kDescriptorRingSize * kDescriptorsPerDispatch;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NodeMask = 0;

  if (FAILED(device->CreateDescriptorHeap(
          &heap_desc, IID_PPV_ARGS(descriptor_heap_.GetAddressOf())))) {
    return false;
  }

  descriptor_size_ =
      device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_HEAP_PROPERTIES heap_props = {};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Alignment = 0;
  buffer_desc.Width = sizeof(ComputeConstants);
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  for (uint32_t i = 0; i < kDescriptorRingSize; ++i) {
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(constant_buffers_[i].GetAddressOf())))) {
      return false;
    }
  }

  device_ = device;
  return true;
}

void ComputeEngine::Destroy() {
  for (auto& buffer : constant_buffers_) {
    buffer.Reset();
  }

  passes_.clear();
  root_signature_.Reset();
  descriptor_heap_.Reset();

  device_ = nullptr;
}

}  // namespace reshade_dlssnr

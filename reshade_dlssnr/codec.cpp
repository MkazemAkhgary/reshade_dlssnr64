#include "codec.h"

#include <reshade.hpp>

namespace reshade_dlssnr {

DXGI_FORMAT DlssNrCodec::TypedFormat(DXGI_FORMAT format) const {
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

void DlssNrCodec::CreateShaderResourceView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
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

void DlssNrCodec::CreateUnorderedAccessView(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  desc.Format = TypedFormat(resource->GetDesc().Format);
  desc.Texture2D.MipSlice = 0;
  desc.Texture2D.PlaneSlice = 0;
  device_->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void DlssNrCodec::CreateConstantBufferView(ID3D12Resource* resource, const void* data, size_t size, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
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

bool DlssNrCodec::Init(ID3D12Device* device) {
  if (device_ != nullptr) return true;
  if (device == nullptr) return false;

  D3D12_DESCRIPTOR_RANGE ranges[3] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = kDlssNrSrvCount;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].RegisterSpace = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = kDlssNrUavCount;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].RegisterSpace = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  ranges[2].NumDescriptors = kDlssNrCbvCount;
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
  if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
    if (error != nullptr) {
      reshade::log::message(reshade::log::level::error, "DLSSNR codec root signature serialization failed");
      error->Release();
    }
    return false;
  }

  if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature_)))) {
    signature->Release();
    return false;
  }
  signature->Release();

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_;
  pso_desc.CS.pShaderBytecode = DlssNr_cso;
  pso_desc.CS.BytecodeLength = sizeof(DlssNr_cso);
  pso_desc.NodeMask = 0;

  if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state_)))) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kDlssNrHeapCount * kDlssNrPerDispatchDescriptors;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  heap_desc.NodeMask = 0;

  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&descriptor_heap_)))) {
    return false;
  }

  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_HEAP_PROPERTIES heap_props = {};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Alignment = 0;
  buffer_desc.Width = sizeof(DlssNrConstants);
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

  for (uint32_t i = 0; i < kDlssNrHeapCount; ++i) {
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buffer_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constant_buffers_[i])))) {
      return false;
    }
  }

  device_ = device;
  return true;
}

bool DlssNrCodec::InitMaskShader(ID3D12Device* device) {
  if (mask_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* mask_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;
    float gMvScaleY;
    uint  gGuideWidth;
    uint  gGuideHeight;
};

Texture2D<float4> gPreUI : register(t0);
Texture2D<float4> gFinal : register(t1);
RWTexture2D<float4> gMask : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float3 pre = gPreUI.SampleLevel(s0, uv, 0).rgb;
    float3 final = gFinal.SampleLevel(s0, uv, 0).rgb;

    float3 diff3 = abs(pre - final);
    // Use the maximum channel difference. UI/HUD elements often differ most in
    // one channel (e.g. white text, green health bars). Luma averaging can
    // understate thin colored UI and leave it translucent.
    float diff = max(diff3.r, max(diff3.g, diff3.b));

    // Soft binary threshold. The lower bound is raised enough to ignore
    // sub-perceptual differences between the clean pass and the final
    // backbuffer (temporal noise, tiny bit-depth differences) while still
    // keeping high-contrast UI/HUD pixels fully opaque.
    float mask = smoothstep(0.02, 0.08, diff);
    gMask[id.xy] = float4(mask, mask, mask, 1.0);
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          mask_shader_source,
          strlen(mask_shader_source),
          nullptr,
          nullptr,
          nullptr,
          "CSMain",
          "cs_5_1",
          0,
          0,
          &shader_blob,
          &error_blob))) {
    if (error_blob != nullptr) error_blob->Release();
    return false;
  }

  if (error_blob != nullptr) error_blob->Release();

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_;
  pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
  pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
  pso_desc.NodeMask = 0;

  const HRESULT hr = device->CreateComputePipelineState(
      &pso_desc,
      IID_PPV_ARGS(&mask_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchMask(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* pre_ui,
    ID3D12Resource* backbuffer,
    ID3D12Resource* out_mask) {
  if (mask_pipeline_state_ == nullptr || command_list == nullptr ||
      pre_ui == nullptr || backbuffer == nullptr || out_mask == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDlssNrHeapCount;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      pre_ui,
      backbuffer,
      pre_ui,
      pre_ui,
      pre_ui,
  };

  for (uint32_t i = 0; i < kDlssNrSrvCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(i) * descriptor_size_;
    CreateShaderResourceView(srvs[i], handle);
  }

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_mask,
      out_mask,
  };

  for (uint32_t i = 0; i < kDlssNrUavCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + i) * descriptor_size_;
    CreateUnorderedAccessView(uavs[i], handle);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = cpu_handle;
  cbv_handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + kDlssNrUavCount) * descriptor_size_;
  CreateConstantBufferView(constant_buffers_[slot], &constants, sizeof(constants), cbv_handle);

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_);
  command_list->SetPipelineState(mask_pipeline_state_);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(
      (constants.Width + 7) / 8,
      (constants.Height + 7) / 8,
      1);

  return true;
}

bool DlssNrCodec::InitCompositeShader(ID3D12Device* device) {
  if (composite_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* composite_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gColourStrength;
    uint  gDebugView;
    float gMaxRatio;
    uint  gPassthrough;
    float gMvScaleX;
    float gMvScaleY;
    uint  gGuideWidth;
    uint  gGuideHeight;
};

Texture2D<float4> gEnhanced : register(t0);
Texture2D<float4> gOriginal : register(t1);
Texture2D<float4> gMask : register(t2);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 enhanced = gEnhanced.SampleLevel(s0, uv, 0);
    float4 original = gOriginal.SampleLevel(s0, uv, 0);
    float mask = gMask.SampleLevel(s0, uv, 0).r;

    float4 output = lerp(enhanced, original, mask);
    output.a = original.a;
    gOutput[id.xy] = output;
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          composite_shader_source,
          strlen(composite_shader_source),
          nullptr,
          nullptr,
          nullptr,
          "CSMain",
          "cs_5_1",
          0,
          0,
          &shader_blob,
          &error_blob))) {
    if (error_blob != nullptr) error_blob->Release();
    return false;
  }

  if (error_blob != nullptr) error_blob->Release();

  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_;
  pso_desc.CS.pShaderBytecode = shader_blob->GetBufferPointer();
  pso_desc.CS.BytecodeLength = shader_blob->GetBufferSize();
  pso_desc.NodeMask = 0;

  const HRESULT hr = device->CreateComputePipelineState(
      &pso_desc,
      IID_PPV_ARGS(&composite_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchComposite(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* enhanced,
    ID3D12Resource* original,
    ID3D12Resource* mask,
    ID3D12Resource* out_target) {
  if (composite_pipeline_state_ == nullptr || command_list == nullptr ||
      enhanced == nullptr || original == nullptr || mask == nullptr ||
      out_target == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDlssNrHeapCount;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      enhanced,
      original,
      mask,
      mask,
      mask,
  };

  for (uint32_t i = 0; i < kDlssNrSrvCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(i) * descriptor_size_;
    CreateShaderResourceView(srvs[i], handle);
  }

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_target,
  };

  for (uint32_t i = 0; i < kDlssNrUavCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + i) * descriptor_size_;
    CreateUnorderedAccessView(uavs[i], handle);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = cpu_handle;
  cbv_handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + kDlssNrUavCount) * descriptor_size_;
  CreateConstantBufferView(constant_buffers_[slot], &constants, sizeof(constants), cbv_handle);

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_);
  command_list->SetPipelineState(composite_pipeline_state_);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(
      (constants.Width + 7) / 8,
      (constants.Height + 7) / 8,
      1);

  return true;
}

bool DlssNrCodec::Dispatch(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* source,
    ID3D12Resource* model,
    ID3D12Resource* original,
    ID3D12Resource* motion,
    ID3D12Resource* prev_edit,
    ID3D12Resource* out_target,
    ID3D12Resource* out_keep) {
  if (!IsInit() || command_list == nullptr || source == nullptr || out_target == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDlssNrHeapCount;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) * kDlssNrPerDispatchDescriptors * descriptor_size_;

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      source,
      model != nullptr ? model : source,
      original != nullptr ? original : source,
      motion != nullptr ? motion : source,
      prev_edit != nullptr ? prev_edit : source,
  };

  for (uint32_t i = 0; i < kDlssNrSrvCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(i) * descriptor_size_;
    CreateShaderResourceView(srvs[i], handle);
  }

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_keep != nullptr ? out_keep : out_target,
  };

  for (uint32_t i = 0; i < kDlssNrUavCount; ++i) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_handle;
    handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + i) * descriptor_size_;
    CreateUnorderedAccessView(uavs[i], handle);
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cbv_handle = cpu_handle;
  cbv_handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + kDlssNrUavCount) * descriptor_size_;
  CreateConstantBufferView(constant_buffers_[slot], &constants, sizeof(constants), cbv_handle);

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_);
  command_list->SetPipelineState(pipeline_state_);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(
      (constants.Width + 7) / 8,
      (constants.Height + 7) / 8,
      1);

  return true;
}

void DlssNrCodec::Destroy() {
  for (auto* buffer : constant_buffers_) {
    if (buffer != nullptr) {
      buffer->Release();
      buffer = nullptr;
    }
  }

  if (pipeline_state_ != nullptr) {
    pipeline_state_->Release();
    pipeline_state_ = nullptr;
  }

  if (root_signature_ != nullptr) {
    root_signature_->Release();
    root_signature_ = nullptr;
  }

  if (mask_pipeline_state_ != nullptr) {
    mask_pipeline_state_->Release();
    mask_pipeline_state_ = nullptr;
  }

  if (composite_pipeline_state_ != nullptr) {
    composite_pipeline_state_->Release();
    composite_pipeline_state_ = nullptr;
  }

  if (descriptor_heap_ != nullptr) {
    descriptor_heap_->Release();
    descriptor_heap_ = nullptr;
  }

  device_ = nullptr;
}

}  // namespace reshade_dlssnr

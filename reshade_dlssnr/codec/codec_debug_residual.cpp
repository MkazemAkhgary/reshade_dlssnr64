#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitDebugResidualShader(ID3D12Device* device) {
  if (debug_residual_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* debug_residual_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    float gWhitePoint;
    uint  gWidth;
    uint  gHeight;
};

Texture2D<float4> gEnhanced : register(t0);
Texture2D<float4> gOriginal : register(t1);
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

    float3 diff = enhanced.rgb - original.rgb;
    float3 shown = saturate(0.5 + diff * 20.0);
    gOutput[id.xy] = float4(shown, 1.0);
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          debug_residual_shader_source,
          strlen(debug_residual_shader_source),
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
      IID_PPV_ARGS(&debug_residual_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchDebugResidual(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* enhanced,
    ID3D12Resource* original,
    ID3D12Resource* out_target) {
  if (debug_residual_pipeline_state_ == nullptr || command_list == nullptr ||
      enhanced == nullptr || original == nullptr || out_target == nullptr) {
    return false;
  }

  const uint32_t slot = heap_index_;
  heap_index_ = (heap_index_ + 1) % kDlssNrHeapCount;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle =
      descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  cpu_handle.ptr += static_cast<SIZE_T>(slot) *
                    kDlssNrPerDispatchDescriptors * descriptor_size_;
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle =
      descriptor_heap_->GetGPUDescriptorHandleForHeapStart();
  gpu_handle.ptr += static_cast<UINT64>(slot) *
                    kDlssNrPerDispatchDescriptors * descriptor_size_;

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      enhanced,
      original,
      enhanced,
      enhanced,
      enhanced,
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
  cbv_handle.ptr += static_cast<SIZE_T>(kDlssNrSrvCount + kDlssNrUavCount) *
                    descriptor_size_;
  CreateConstantBufferView(
      constant_buffers_[slot], &constants, sizeof(constants), cbv_handle);

  ID3D12DescriptorHeap* heaps[] = {descriptor_heap_};
  command_list->SetDescriptorHeaps(1, heaps);
  command_list->SetComputeRootSignature(root_signature_);
  command_list->SetPipelineState(debug_residual_pipeline_state_);
  command_list->SetComputeRootDescriptorTable(0, gpu_handle);
  command_list->Dispatch(
      (constants.Width + 7) / 8,
      (constants.Height + 7) / 8,
      1);

  return true;
}

}  // namespace reshade_dlssnr

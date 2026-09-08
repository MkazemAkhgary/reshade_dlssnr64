#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitDownsampleShader(ID3D12Device* device) {
  if (downsample_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* downsample_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    uint  gWidth;
    uint  gHeight;
};

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    gOutput[id.xy] = color;
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          downsample_shader_source,
          strlen(downsample_shader_source),
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
      IID_PPV_ARGS(&downsample_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchDownsample(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* source,
    ID3D12Resource* out_target) {
  if (downsample_pipeline_state_ == nullptr || command_list == nullptr ||
      source == nullptr || out_target == nullptr) {
    return false;
  }

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      source,
      source,
      source,
      source,
      source,
  };

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_target,
  };

  return BindAndDispatch(
      command_list, downsample_pipeline_state_, constants, srvs, uavs);
}

bool DlssNrCodec::InitUpsampleResidualShader(ID3D12Device* device) {
  if (upsample_residual_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* upsample_residual_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gMode;
    uint  gWidth;
    uint  gHeight;
    float gTransferStrength;
    float gLumaStrength;
    float gChromaStrength;
    uint  gDebugView;
    float gUiMaskThreshold;
    float gUiMaskSoftness;
    float gResidualSuppression;
    float gResidualThreshold;
};

Texture2D<float4> gOriginal : register(t0);
Texture2D<float4> gLowInput : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 original = gOriginal.SampleLevel(s0, uv, 0);
    float4 low_input = gLowInput.SampleLevel(s0, uv, 0);
    float4 low_enhanced = gLowEnhanced.SampleLevel(s0, uv, 0);

    float low_input_luma = dot(low_input.rgb, float3(0.2126, 0.7152, 0.0722));
    float low_enhanced_luma = dot(low_enhanced.rgb, float3(0.2126, 0.7152, 0.0722));

    float final_luma = lerp(low_input_luma, low_enhanced_luma, gLumaStrength);
    float luma_gain = final_luma / max(low_input_luma, 1e-4);
    luma_gain = clamp(luma_gain, 0.25, 4.0);

    float dark_dampen =
        smoothstep(0.0, max(gResidualThreshold, 0.001), low_input_luma);
    float suppression = lerp(1.0, dark_dampen, gResidualSuppression);

    luma_gain = lerp(luma_gain, 1.0, (1.0 - suppression) * gResidualSuppression);

    float chroma_amount = gChromaStrength * suppression;

    float3 low_input_chroma = low_input.rgb / max(low_input_luma, 1e-3);
    float3 low_enhanced_chroma = low_enhanced.rgb / max(low_enhanced_luma, 1e-3);

    float3 chroma_ratio = lerp(
        float3(1.0, 1.0, 1.0),
        low_enhanced_chroma / max(low_input_chroma, 1e-3),
        chroma_amount);

    float3 result = original.rgb * luma_gain * chroma_ratio;

    gOutput[id.xy] = float4(result, original.a);
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          upsample_residual_shader_source,
          strlen(upsample_residual_shader_source),
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
      IID_PPV_ARGS(&upsample_residual_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchUpsampleResidual(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* original,
    ID3D12Resource* low_input,
    ID3D12Resource* low_enhanced,
    ID3D12Resource* out_target) {
  if (upsample_residual_pipeline_state_ == nullptr || command_list == nullptr ||
      original == nullptr || low_input == nullptr || low_enhanced == nullptr ||
      out_target == nullptr) {
    return false;
  }

  const ID3D12Resource* srvs[kDlssNrSrvCount] = {
      original,
      low_input,
      low_enhanced,
      low_input,
      low_enhanced,
  };

  const ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_target,
  };

  return BindAndDispatch(
      command_list, upsample_residual_pipeline_state_, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr

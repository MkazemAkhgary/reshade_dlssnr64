#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitLumaColorShader(ID3D12Device* device) {
  if (luma_color_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* luma_color_shader_source = R"(
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
    float gMvScaleX;
    float gMvScaleY;
    uint  gGuideWidth;
    uint  gGuideHeight;
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

    float original_luma = dot(original.rgb, float3(0.2126, 0.7152, 0.0722));
    float enhanced_luma = dot(enhanced.rgb, float3(0.2126, 0.7152, 0.0722));

    float final_luma = lerp(original_luma, enhanced_luma, gLumaStrength);

    float3 original_chroma = original.rgb / max(original_luma, 1e-6);
    float3 enhanced_chroma = enhanced.rgb / max(enhanced_luma, 1e-6);
    float3 final_chroma = lerp(original_chroma, enhanced_chroma, gChromaStrength);

    float3 color_result = final_luma * final_chroma;

    gOutput[id.xy] = float4(color_result, original.a);
}
)";

  ID3DBlob* shader_blob = nullptr;
  ID3DBlob* error_blob = nullptr;

  if (FAILED(D3DCompile(
          luma_color_shader_source,
          strlen(luma_color_shader_source),
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
      IID_PPV_ARGS(&luma_color_pipeline_state_));

  shader_blob->Release();
  return SUCCEEDED(hr);
}

bool DlssNrCodec::DispatchLumaColor(
    ID3D12GraphicsCommandList* command_list,
    const DlssNrConstants& constants,
    ID3D12Resource* enhanced,
    ID3D12Resource* original,
    ID3D12Resource* out_target) {
  if (luma_color_pipeline_state_ == nullptr || command_list == nullptr ||
      enhanced == nullptr || original == nullptr || out_target == nullptr) {
    return false;
  }

  const ID3D12Resource* srvs[kDlssNrSrvCount] = {
      enhanced,
      original,
      enhanced,
      enhanced,
      enhanced,
  };

  const ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_target,
  };

  return BindAndDispatch(
      command_list, luma_color_pipeline_state_, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr

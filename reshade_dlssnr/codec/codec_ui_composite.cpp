#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitCompositeShader(ID3D12Device* device) {
  if (composite_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* composite_shader_source = R"(
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

  const ID3D12Resource* srvs[kDlssNrSrvCount] = {
      enhanced,
      original,
      mask,
      mask,
      mask,
  };

  const ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
      out_target,
  };

  return BindAndDispatch(
      command_list, composite_pipeline_state_, constants, srvs, uavs);
}

}  // namespace reshade_dlssnr

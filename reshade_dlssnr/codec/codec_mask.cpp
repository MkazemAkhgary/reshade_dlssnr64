#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitMaskShader(ID3D12Device* device) {
  if (mask_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* mask_shader_source = R"(
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
    float diff = max(diff3.r, max(diff3.g, diff3.b));

    float lower = gUiMaskThreshold;
    float upper = lower + gUiMaskSoftness;
    float mask = smoothstep(lower, upper, diff);
    gMask[id.xy] = float4(mask, mask, mask, 1.0);
}
)";

  return CreateComputePipelineState(
      device, mask_shader_source, &mask_pipeline_state_);
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

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      pre_ui,
      backbuffer,
      pre_ui,
      pre_ui,
      pre_ui,
  };

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_mask,
      out_mask,
  };

  return BindAndDispatch(
      command_list, mask_pipeline_state_.Get(), constants, srvs, uavs);
}

}  // namespace reshade_dlssnr

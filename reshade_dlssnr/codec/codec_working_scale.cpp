#include "codec.h"

namespace reshade_dlssnr {

bool DlssNrCodec::InitDownsampleShader(ID3D12Device* device) {
  if (downsample_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* downsample_shader_source = R"(
cbuffer Params : register(b0)
{
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

  return CreateComputePipelineState(
      device, downsample_shader_source, downsample_pipeline_state_.GetAddressOf());
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
  };

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
  };

  return BindAndDispatch(
      command_list, downsample_pipeline_state_.Get(), constants, srvs, uavs);
}

bool DlssNrCodec::InitUpsampleResidualShader(ID3D12Device* device) {
  if (upsample_residual_pipeline_state_ != nullptr) return true;
  if (root_signature_ == nullptr) return false;

  const char* upsample_residual_shader_source = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float gLumaStrength;
    float gChromaStrength;
    uint  gSoftCompression;
    float gSoftMaxBrighten;
    float gSoftMaxDarken;
    float gSoftMaxChroma;
    float gSoftKnee;
};

Texture2D<float4> gOriginal : register(t0);
Texture2D<float4> gLowInput : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float SoftCompress(float x, float max_pos, float max_neg, float knee)
{
    knee = max(knee, 1e-6);
    if (x >= 0.0)
        return max_pos * x / (max_pos + knee * abs(x));
    else
        return max_neg * x / (max_neg + knee * abs(x));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 original = gOriginal.SampleLevel(s0, uv, 0);
    float4 low_input = gLowInput.SampleLevel(s0, uv, 0);
    float4 low_enhanced = gLowEnhanced.SampleLevel(s0, uv, 0);

    float3 in_rgb = low_input.rgb;
    float3 out_rgb = low_enhanced.rgb;

    float3 residual;
    if (gSoftCompression != 0) {
        float in_luma = max(dot(in_rgb, float3(0.2126, 0.7152, 0.0722)), 1e-6);
        float out_luma = max(dot(out_rgb, float3(0.2126, 0.7152, 0.0722)), 1e-6);

        float residual_luma = log(out_luma) - log(in_luma);
        float3 residual_chroma =
            log(out_rgb / out_luma) - log(in_rgb / in_luma);

        residual_luma = SoftCompress(
            residual_luma, gSoftMaxBrighten, gSoftMaxDarken, gSoftKnee);
        residual_chroma.x = SoftCompress(
            residual_chroma.x, gSoftMaxChroma, gSoftMaxChroma, gSoftKnee);
        residual_chroma.y = SoftCompress(
            residual_chroma.y, gSoftMaxChroma, gSoftMaxChroma, gSoftKnee);
        residual_chroma.z = SoftCompress(
            residual_chroma.z, gSoftMaxChroma, gSoftMaxChroma, gSoftKnee);

        float edited_luma = in_luma * exp(residual_luma);
        float3 edited_chroma =
            exp(log(in_rgb / in_luma) + residual_chroma);
        float3 edited_rgb = edited_luma * edited_chroma;

        residual = edited_rgb - in_rgb;
    } else {
        residual = out_rgb - in_rgb;
    }

    float3 edited = original.rgb + residual;

    // Final luma/chroma strength blend (after soft compression).
    // Use linear YCbCr (BT.709) color-difference space to avoid division
    // by luma, which causes artifacts in dark areas. No clamping needed.
    float original_y = dot(original.rgb, float3(0.2126, 0.7152, 0.0722));
    float edited_y   = dot(edited,   float3(0.2126, 0.7152, 0.0722));
    float final_y = lerp(original_y, edited_y, gLumaStrength);

    float original_cb = (original.b - original_y) / 1.8556;
    float original_cr = (original.r - original_y) / 1.5748;
    float edited_cb   = (edited.b   - edited_y)   / 1.8556;
    float edited_cr   = (edited.r   - edited_y)   / 1.5748;

    float final_cb = lerp(original_cb, edited_cb, gChromaStrength);
    float final_cr = lerp(original_cr, edited_cr, gChromaStrength);

    float3 final_rgb;
    final_rgb.r = final_y + 1.5748 * final_cr;
    final_rgb.g = final_y - 0.1873 * final_cb - 0.4681 * final_cr;
    final_rgb.b = final_y + 1.8556 * final_cb;

    gOutput[id.xy] = float4(final_rgb, original.a);
}
)";

  return CreateComputePipelineState(
      device, upsample_residual_shader_source, upsample_residual_pipeline_state_.GetAddressOf());
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

  ID3D12Resource* srvs[kDlssNrSrvCount] = {
      original,
      low_input,
      low_enhanced,
  };

  ID3D12Resource* uavs[kDlssNrUavCount] = {
      out_target,
  };

  return BindAndDispatch(
      command_list, upsample_residual_pipeline_state_.Get(), constants, srvs, uavs);
}

}  // namespace reshade_dlssnr

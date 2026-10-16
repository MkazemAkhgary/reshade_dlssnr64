#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/luma.hlsli"

#define kLumaAbs          shader.Params0.x
#define kLumaRel          shader.Params0.y
#define kChromaAbs        shader.Params0.z
#define kMaskEditStrength shader.Params1.x
#define kHistoryValid     shader.Params1.w

Texture2D<float4> gInput : register(t0);
Texture2D<float4> gOutput : register(t1);
Texture2D<float> gPreviousMask : register(t2);
RWTexture2D<float> gRawMask : register(u0);

float LoadPreviousMask(int2 xy, uint2 previous_size)
{
    const float2 uv = (float2(xy) + 0.5) / float2(Width, Height);
    const uint2 previous_xy = min(
        uint2(uv * float2(previous_size)), previous_size - uint2(1u, 1u));
    return gPreviousMask.Load(int3(previous_xy, 0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    uint previous_width = 0;
    uint previous_height = 0;
    gPreviousMask.GetDimensions(previous_width, previous_height);
    const uint2 previous_size = max(
        uint2(previous_width, previous_height), uint2(1u, 1u));

    const float3 a = gInput.Load(int3(id.xy, 0)).rgb;
    const float3 b = gOutput.Load(int3(id.xy, 0)).rgb;
    const float previous = kHistoryValid > 0.5
        ? LoadPreviousMask(int2(id.xy), previous_size) : 0.0;
    const float hysteresis = lerp(1.0, 0.5, saturate(previous * 2.0));
    const float a_luma = dot(a, kLuma709);
    const float b_luma = dot(b, kLuma709);
    const float luma_diff = abs(b_luma - a_luma) * kMaskEditStrength;
    const float luma_ref = max(abs(a_luma), abs(b_luma));
    const float3 chroma_diff = (b - b_luma) - (a - a_luma);
    const bool luma_edit =
        luma_diff > (kLumaAbs * hysteresis) &&
        luma_diff > (kLumaRel * luma_ref * hysteresis);
    const bool chroma_edit =
        length(chroma_diff) * kMaskEditStrength > (kChromaAbs * hysteresis);
    gRawMask[id.xy] = (luma_edit || chroma_edit) ? 1.0 : 0.0;
}

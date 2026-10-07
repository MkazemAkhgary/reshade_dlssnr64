#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);


float3 DecodeSource(float3 c)
{
    int fmt = (int)(kGradeFormat + 0.5);

    if (fmt == 1)
        return max(c, 0.0) * (80.0 / 100.0);

    if (fmt == 2)
        return PqToNits(c) / 100.0;

    // Code 3 is the gain-map path's linear SDR model domain.
    if (fmt == 3)
        return c;

    return SrgbToLinearExt(c);
}

float3 EncodeSource(float3 lin)
{
    int fmt = (int)(kGradeFormat + 0.5);

    if (fmt == 1)
        return max(lin, 0.0) * (100.0 / 80.0);

    if (fmt == 2)
        return NitsToPq(max(lin, 0.0) * 100.0);

    // Code 3 is already linear SDR.
    if (fmt == 3)
        return lin;

    // Standard SDR sRGB encoding.
    //
    // Values below zero cannot be represented in SDR.
    // Values above one cannot be represented either, so the final saturate()
    // is the correct gamut/range operation rather than a creative shoulder.
    return saturate(LinearToSrgbExt(max(lin, 0.0)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv =
        (float2(id.xy) + 0.5) /
        float2(Width, Height);

    float4 src =
        gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeSource(src.rgb);
    float3 graded = GradeLinearFwd(lin);

    gOutput[id.xy] =
        float4(EncodeSource(graded), src.a);
}

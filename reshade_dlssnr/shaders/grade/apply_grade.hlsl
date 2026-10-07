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
    float3 result = SrgbToLinearExt(c);

    if (fmt == 1)
        result = max(c, 0.0) *
                 (kScRgbReferenceWhiteNits / kSdrReferenceWhiteNits);
    else if (fmt == 2)
        result = PqToNits(c) / kSdrReferenceWhiteNits;
    else if (fmt == 3)
        result = c;

    return result;
}

float3 EncodeSource(float3 lin)
{
    int fmt = (int)(kGradeFormat + 0.5);
    float3 result = saturate(LinearToSrgbExt(max(lin, 0.0)));

    if (fmt == 1)
        result = max(lin, 0.0) *
                 (kSdrReferenceWhiteNits / kScRgbReferenceWhiteNits);
    else if (fmt == 2)
        result = NitsToPq(max(lin, 0.0) * kSdrReferenceWhiteNits);
    else if (fmt == 3)
        result = lin;

    return result;
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

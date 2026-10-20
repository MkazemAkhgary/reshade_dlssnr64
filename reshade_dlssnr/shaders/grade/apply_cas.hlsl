#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"

#define A_GPU 1
#define A_HLSL 1
#include "../../third_party/FidelityFX-CAS/ffx-cas/ffx_a.h"

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

AF3 CasLoad(ASU2 pixel)
{
    return gSource.Load(int3(pixel, 0)).rgb;
}

void CasInput(inout AF1 r, inout AF1 g, inout AF1 b)
{
    const float3 linear_rgb = SrgbToLinearExt(float3(r, g, b));
    r = linear_rgb.r;
    g = linear_rgb.g;
    b = linear_rgb.b;
}

#include "../../third_party/FidelityFX-CAS/ffx-cas/ffx_cas.h"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    const uint2 pixel = id.xy;
    const float4 src = gSource.Load(int3(pixel, 0));
    const AU4 const0 = grade.CasConst0;
    const AU4 const1 = grade.CasConst1;
    AF1 r = 0.0;
    AF1 g = 0.0;
    AF1 b = 0.0;
    CasFilter(r, g, b, pixel, const0, const1, true);
    const float3 encoded = LinearToSrgbExt(saturate(float3(r, g, b)));
    gOutput[pixel] = float4(encoded, src.a);
}

#include "common/cbuffer.hlsli"

Texture2D<float4> gUnused0 : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWBuffer<uint> gStats : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    gStats[0] = 0u; // invalid pixel count
    gStats[1] = 0u; // valid reprojection sample marker
}

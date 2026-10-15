#include "common/cbuffer.hlsli"

Texture2D<float4> gCandidate : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWBuffer<uint> gStats : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float code = gCandidate.Load(int3(id.xy, 0)).a;
    if (code != 1.0) {
        uint ignored;
        InterlockedAdd(gStats[0], 1u, ignored);
    }
    if (id.x == 0 && id.y == 0) {
        uint ignored;
        InterlockedOr(gStats[1], 1u, ignored);
    }
}

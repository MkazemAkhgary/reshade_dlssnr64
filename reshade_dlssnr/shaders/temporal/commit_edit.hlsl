#include "common/cbuffer.hlsli"

Texture2D<float4> gCandidate : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWTexture2D<float4> gHistoryEdit : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float4 candidate = gCandidate.Load(int3(id.xy, 0));
    const bool can_commit = candidate.a == 1.0 || candidate.a == 2.0;
    gHistoryEdit[id.xy] = float4(can_commit ? clamp(candidate.rgb, -0.69314718056, 0.69314718056)
                                             : float3(0.0, 0.0, 0.0),
                                 1.0);
}

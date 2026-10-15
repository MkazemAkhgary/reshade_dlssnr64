#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"

Texture2D<float> gExposureMap : register(t0);
RWTexture2D<float> gPartialSums : register(u0);

groupshared float gSum[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID,
            uint3 group_id : SV_GroupID,
            uint3 group_thread_id : SV_GroupThreadID)
{
    const uint local_index = group_thread_id.x + group_thread_id.y * 8;
    float value = 0.0;
    if (id.x < Width && id.y < Height)
        value = gExposureMap.Load(int3(id.xy, 0));
    gSum[local_index] = value;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (local_index < stride)
            gSum[local_index] += gSum[local_index + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (local_index == 0)
        gPartialSums[group_id.xy] = gSum[0];
}

#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"

Texture2D<float> gPartialSums : register(t0);
RWTexture2D<float> gMeanExposure : register(u0);

groupshared float gSum[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 group_thread_id : SV_GroupThreadID)
{
    const uint local_index = group_thread_id.x + group_thread_id.y * 8;
    const uint partial_width = (uint)(shader.Params0.x + 0.5);
    const uint partial_height = (uint)(shader.Params0.y + 0.5);
    const uint partial_count = partial_width * partial_height;
    float local_sum = 0.0;

    for (uint i = local_index; i < partial_count; i += 64)
    {
        const uint2 xy = uint2(i % partial_width, i / partial_width);
        local_sum += gPartialSums.Load(int3(xy, 0));
    }
    gSum[local_index] = local_sum;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (local_index < stride)
            gSum[local_index] += gSum[local_index + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (local_index == 0)
    {
        const float pixel_count = (float)Width * (float)Height;
        gMeanExposure[uint2(0, 0)] = gSum[0] / max(pixel_count, 1.0);
    }
}

#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"

Texture2D<float4> gLocalCarrier : register(t0);
Texture2D<float4> gGlobalCarrier : register(t1);
RWTexture2D<float> gPartialSums : register(u0);

 groupshared float gSum[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID,
            uint3 group_id : SV_GroupID,
            uint3 group_thread_id : SV_GroupThreadID)
{
    const uint local_index = group_thread_id.x + group_thread_id.y * 8;
    float value = 0.0;
    if (id.x < Width && id.y < Height) {
        const float local_inverse_scale = gLocalCarrier.Load(int3(id.xy, 0)).a;
        const float global_inverse_scale = gGlobalCarrier.Load(int3(id.xy, 0)).a;
        if (isfinite(local_inverse_scale) && isfinite(global_inverse_scale) &&
            local_inverse_scale > 0.0 && global_inverse_scale > 0.0) {
            value = global_inverse_scale / local_inverse_scale;
        } else {
            value = 1.0;
        }
    }
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

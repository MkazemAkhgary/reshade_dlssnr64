#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"

Texture2D<float4> gPacked : register(t0);
RWBuffer<uint> gStats : register(u0);

#define kGainMinLog2 hdr.GainMinLog2
#define kGainMaxLog2 hdr.GainMaxLog2

groupshared uint gMin[64];
groupshared uint gMax[64];
groupshared uint gExcursions[64];
groupshared uint gOutOfRange[64];

uint SortableFloat(float value)
{
    const uint bits = asuint(value);
    return (bits & 0x80000000u) != 0u ? ~bits : (bits ^ 0x80000000u);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID,
            uint3 group_thread_id : SV_GroupThreadID)
{
    const uint local_index = group_thread_id.x + group_thread_id.y * 8u;
    uint local_min = 0xffffffffu;
    uint local_max = 0u;
    uint local_excursions = 0u;
    uint local_out_of_range = 0u;

    if (id.x < Width && id.y < Height) {
        const float4 packed = gPacked.Load(int3(id.xy, 0));
        const float scale = packed.a;
        if (!(scale > 0.0) || !isfinite(scale) || scale > 1.0e20) {
            local_excursions = 1u;
        } else {
            const float gain_log2 = log2(scale);
            if (!isfinite(gain_log2) || abs(gain_log2) > 1.0e20) {
                local_excursions = 1u;
            } else {
                const uint sortable = SortableFloat(gain_log2);
                local_min = sortable;
                local_max = sortable;
                if (packed.r < 0.0 || packed.r > 1.0 ||
                    packed.g < 0.0 || packed.g > 1.0 ||
                    packed.b < 0.0 || packed.b > 1.0) {
                    local_excursions = 1u;
                }
                if (gain_log2 < kGainMinLog2 || gain_log2 > kGainMaxLog2)
                    local_out_of_range = 1u;
            }
        }
    }

    gMin[local_index] = local_min;
    gMax[local_index] = local_max;
    gExcursions[local_index] = local_excursions;
    gOutOfRange[local_index] = local_out_of_range;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (local_index < stride) {
            gMin[local_index] = min(gMin[local_index], gMin[local_index + stride]);
            gMax[local_index] = max(gMax[local_index], gMax[local_index + stride]);
            gExcursions[local_index] += gExcursions[local_index + stride];
            gOutOfRange[local_index] += gOutOfRange[local_index + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (local_index == 0u) {
        InterlockedMin(gStats[0], gMin[0]);
        InterlockedMax(gStats[1], gMax[0]);
        InterlockedAdd(gStats[2], gExcursions[0]);
        InterlockedAdd(gStats[3], gOutOfRange[0]);
    }
}

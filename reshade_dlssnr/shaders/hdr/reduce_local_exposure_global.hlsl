#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"

Texture2D<float> gTileLogScales : register(t0);
RWTexture2D<float> gGlobalScale : register(u0);

groupshared float gWeightedStops[64];
groupshared float gPixelWeights[64];

[numthreads(8, 8, 1)]
void CSMain(uint3 group_thread_id : SV_GroupThreadID)
{
    const uint lane = group_thread_id.x + group_thread_id.y * 8u;
    const uint tile_count = Width * Height;
    const uint source_width = max((uint)shader.Params0.x, 1u);
    const uint source_height = max((uint)shader.Params0.y, 1u);
    float weighted_stops = 0.0;
    float pixel_weights = 0.0;

    for (uint i = lane; i < tile_count; i += 64u) {
        const uint2 tile = uint2(i % Width, i / Width);
        const float tile_stops = gTileLogScales.Load(int3(tile, 0));
        const uint tile_x0 = tile.x * 8u;
        const uint tile_y0 = tile.y * 8u;
        const uint tile_w = min(8u, source_width - tile_x0);
        const uint tile_h = min(8u, source_height - tile_y0);
        const float pixels = (float)(tile_w * tile_h);
        weighted_stops += tile_stops * pixels;
        pixel_weights += pixels;
    }

    gWeightedStops[lane] = weighted_stops;
    gPixelWeights[lane] = pixel_weights;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            gWeightedStops[lane] += gWeightedStops[lane + stride];
            gPixelWeights[lane] += gPixelWeights[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u) {
        const float mean_stops = gPixelWeights[0] > 0.0
            ? gWeightedStops[0] / gPixelWeights[0]
            : 0.0;
        gGlobalScale[uint2(0, 0)] = exp2(mean_stops);
    }
}

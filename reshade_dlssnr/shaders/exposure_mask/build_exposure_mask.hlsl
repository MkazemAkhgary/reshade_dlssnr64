#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"

#define kMaskSmoothing shader.Params1.y
#define kMaskAlpha     shader.Params1.z
#define kHistoryValid  shader.Params1.w

Texture2D<float> gRawMask : register(t0);
Texture2D<float> gUnused1 : register(t1);
Texture2D<float> gPreviousMask : register(t2);
RWTexture2D<float> gMask : register(u0);

float LoadPreviousMask(int2 xy, uint2 previous_size)
{
    const float2 uv = (float2(xy) + 0.5) / float2(Width, Height);
    const uint2 previous_xy = min(
        uint2(uv * float2(previous_size)), previous_size - uint2(1u, 1u));
    return gPreviousMask.Load(int3(previous_xy, 0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    uint previous_width = 0;
    uint previous_height = 0;
    gPreviousMask.GetDimensions(previous_width, previous_height);
    const uint2 previous_size = max(
        uint2(previous_width, previous_height), uint2(1u, 1u));

    const int radius = (int)(kMaskSmoothing * 2.0 + 0.5);
    const int2 center = int2(id.xy);
    int hits = 0;
    int total = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            const int2 pixel = center + int2(dx, dy);
            if (pixel.x < 0 || pixel.y < 0 || pixel.x >= (int)Width ||
                pixel.y >= (int)Height) continue;
            ++total;
            if (gRawMask.Load(int3(pixel, 0)) >= 0.5) ++hits;
        }
    }

    const float vote = (total > 0 && hits * 2 >= total) ? 1.0 : 0.0;
    const float previous = kHistoryValid > 0.5
        ? LoadPreviousMask(int2(id.xy), previous_size) : 0.0;
    gMask[id.xy] = kHistoryValid > 0.5
        ? lerp(previous, vote, kMaskAlpha) : vote;
}

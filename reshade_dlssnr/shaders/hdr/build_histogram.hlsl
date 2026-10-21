#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/bindings.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"

#define kHistogramFormat shader.Params0.x
#define kLumaMinLog2     shader.Params0.y
#define kLumaMaxLog2     shader.Params0.z
#define kAdaptiveFilterFlags ((uint)(histogram.AdaptiveFilter.x + 0.5))
#define kAdaptiveDepthNear histogram.AdaptiveFilter.y
#define kAdaptiveDepthFar  histogram.AdaptiveFilter.z
#define kDepthInverted     (histogram.AdaptiveFilter.w > 0.5)
#define kSampleStride      histogram.Settings.x
#define kPixelsPerThreadAxis histogram.Settings.y

Texture2D<float4> gSource : register(t0);
Texture2D<float> gMask : register(t1);
Texture2D<float> gDepth : register(t2);
RWBuffer<uint> gHistogram : register(u0);
RWBuffer<uint> gHistogramInactive : register(u1);
SamplerState s0 : register(s0);

groupshared uint gFullHistogram[256];
groupshared uint gAdaptiveHistogram[256];

uint2 ToResourcePixel(uint2 pixel, uint2 source_size, uint2 resource_size)
{
    return min(uint2((float2(pixel) + 0.5) *
                     (float2(resource_size) / float2(max(source_size, 1u)))),
               resource_size - 1u);
}

uint FilteredContribution(uint flags, float depth_near, float depth_far,
                          float mask_value, float depth_value)
{
    uint contribution = 1u;
    if ((flags & 1u) != 0u)
        contribution = (uint)round(saturate(mask_value) * 16.0);
    if (contribution > 0u && (flags & 2u) != 0u &&
        (depth_value < depth_near || depth_value > depth_far))
        contribution = 0u;
    return contribution;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gtid : SV_GroupThreadID,
            uint3 gid : SV_GroupID)
{
    const uint local_index = gtid.x + gtid.y * 8u;
    for (uint i = local_index; i < 256u; i += 64u) {
        gFullHistogram[i] = 0u;
        gAdaptiveHistogram[i] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint stride = max((uint)kSampleStride, 1u);
    const uint block_per_thread = max((uint)kPixelsPerThreadAxis, 1u);
    const uint block_size = 8u * block_per_thread;
    const uint base_x = gid.x * block_size + gtid.x * block_per_thread;
    const uint base_y = gid.y * block_size + gtid.y * block_per_thread;

    [loop]
    for (uint dy = 0u; dy < block_per_thread; dy += stride) {
        [loop]
        for (uint dx = 0u; dx < block_per_thread; dx += stride) {
            const uint2 pixel = uint2(base_x + dx, base_y + dy);
            if (pixel.x >= Width || pixel.y >= Height) continue;

            const float3 encoded = gSource.Load(int3(pixel, 0)).rgb;
            const float3 linear_rgb = DecodeToLinear(
                encoded, (int)(kHistogramFormat + 0.5), kSdrReferenceWhiteNits);
            const float3 luma_weights = ((int)(kHistogramFormat + 0.5) == 2)
                ? kLuma2020 : kLuma709;
            const float luma = dot(max(linear_rgb, 0.0), luma_weights);
            const float l2 = log2(max(luma, 1e-8));
            const float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
            const float t = saturate((l2 - kLumaMinLog2) / span);
            const uint bin = min((uint)(t * 255.0 + 0.5), 255u);

            float mask_value = 1.0;
            if ((kAdaptiveFilterFlags & 1u) != 0u) {
                uint mask_width, mask_height;
                gMask.GetDimensions(mask_width, mask_height);
                if (mask_width > 0u && mask_height > 0u) {
                    const uint2 mask_xy = ToResourcePixel(
                        pixel, uint2(Width, Height), uint2(mask_width, mask_height));
                    mask_value = gMask.Load(int3(mask_xy, 0));
                } else {
                    mask_value = 0.0;
                }
            }

            float depth_value = 0.5;
            if ((kAdaptiveFilterFlags & 2u) != 0u) {
                uint depth_width, depth_height;
                gDepth.GetDimensions(depth_width, depth_height);
                if (depth_width > 0u && depth_height > 0u) {
                    const uint2 depth_xy = ToResourcePixel(
                        pixel, uint2(Width, Height), uint2(depth_width, depth_height));
                    depth_value = saturate(gDepth.Load(int3(depth_xy, 0)));
                    if (kDepthInverted) depth_value = 1.0 - depth_value;
                } else {
                    depth_value = 0.0;
                }
            }

            InterlockedAdd(gFullHistogram[bin], 1u);
            const uint adaptive_contribution = FilteredContribution(
                kAdaptiveFilterFlags, kAdaptiveDepthNear, kAdaptiveDepthFar,
                mask_value, depth_value);
            if (adaptive_contribution > 0u)
                InterlockedAdd(gAdaptiveHistogram[bin], adaptive_contribution);
        }
    }

    // Clear the inactive buffer (the one that will be active next frame).
    // Only one group needs to do this; the buffer is 512 uints and this
    // group has 64 threads, so each thread handles 8 bins.
    if (gid.x == 0u && gid.y == 0u) {
        for (uint i = local_index; i < 256u; i += 64u) {
            gHistogramInactive[i] = 0u;
            gHistogramInactive[256u + i] = 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint index = local_index; index < 256u; index += 64u) {
        const uint full = gFullHistogram[index];
        const uint adaptive = gAdaptiveHistogram[index];
        if (full == 0u && adaptive == 0u) continue;
        if (full > 0u) InterlockedAdd(gHistogram[index], full);
        if (adaptive > 0u) InterlockedAdd(gHistogram[256u + index], adaptive);
    }
}

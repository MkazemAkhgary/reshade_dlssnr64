#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"

#define kFormat          shader.Params0.x
#define kLumaMinLog2     shader.Params0.y
#define kLumaMaxLog2     shader.Params0.z
#define kFilterFlags     ((uint)(shader.Params0.w + 0.5))
#define kDepthNear       shader.Params1.x
#define kDepthFar        shader.Params1.y
#define kDepthInverted   shader.Params1.z
#define kHistogramOffset shader.Params1.w

Texture2D<float4> gSource : register(t0);
Texture2D<float> gMask : register(t1);
Texture2D<float> gDepth : register(t2);
RWBuffer<uint> gHistogram : register(u0);
SamplerState s0 : register(s0);

groupshared uint gSharedHist[256];

uint2 ToResourcePixel(uint2 pixel, uint2 source_size, uint2 resource_size)
{
    return min(uint2((float2(pixel) + 0.5) *
                     (float2(resource_size) / float2(max(source_size, 1u)))),
               resource_size - 1u);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gtid : SV_GroupThreadID,
            uint3 dtid : SV_DispatchThreadID)
{
    const uint local_index = gtid.x + gtid.y * 8u;
    for (uint i = local_index; i < 256u; i += 64u)
        gSharedHist[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < Width && dtid.y < Height) {
        const float3 encoded = gSource.Load(int3(dtid.xy, 0)).rgb;
        const float3 lin = DecodeToLinear(
            encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
        const float3 luma_weights = ((int)(kFormat + 0.5) == 2)
            ? kLuma2020 : kLuma709;
        const float luma = dot(max(lin, 0.0), luma_weights);

        uint contribution = 1u;
        if ((kFilterFlags & 1u) != 0u) {
            uint mask_width, mask_height;
            gMask.GetDimensions(mask_width, mask_height);
            const uint2 mask_xy = ToResourcePixel(
                dtid.xy, uint2(Width, Height), uint2(mask_width, mask_height));
            const float mask_value = saturate(gMask.Load(int3(mask_xy, 0)));
            contribution = (uint)round(mask_value * 16.0);
        }

        if (contribution > 0u && (kFilterFlags & 2u) != 0u) {
            uint depth_width, depth_height;
            gDepth.GetDimensions(depth_width, depth_height);
            const uint2 depth_xy = ToResourcePixel(
                dtid.xy, uint2(Width, Height), uint2(depth_width, depth_height));
            float depth = saturate(gDepth.Load(int3(depth_xy, 0)));
            if (kDepthInverted > 0.5) depth = 1.0 - depth;
            if (depth < kDepthNear || depth > kDepthFar) contribution = 0u;
        }

        if (contribution > 0u) {
            const float l2 = log2(max(luma, 1e-8));
            const float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
            const float t = saturate((l2 - kLumaMinLog2) / span);
            const uint bin = min((uint)(t * 255.0 + 0.5), 255u);
            InterlockedAdd(gSharedHist[bin], contribution);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    for (uint index = local_index; index < 256u; index += 64u) {
        if (gSharedHist[index] > 0u) {
            InterlockedAdd(
                gHistogram[index + (uint)(kHistogramOffset + 0.5)],
                gSharedHist[index]);
        }
    }
}

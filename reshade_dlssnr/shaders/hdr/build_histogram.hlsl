#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"

#define kFormat      shader.Params0.x
#define kLumaMinLog2 shader.Params0.y
#define kLumaMaxLog2 shader.Params0.z
#define kMaskWidth   shader.Params1.x
#define kMaskHeight  shader.Params1.y
#define kUseMask     shader.Params1.z
#define kHistogramOffset shader.Params1.w

Texture2D<float4> gSource : register(t0);
Texture2D<float>  gMask   : register(t1);
RWBuffer<uint>    gHistogram : register(u0);
SamplerState      s0 : register(s0);

groupshared uint gSharedHist[256];

[numthreads(8, 8, 1)]
void CSMain(uint3 gtid : SV_GroupThreadID,
            uint3 dtid : SV_DispatchThreadID)
{
    const uint lidx = gtid.x + gtid.y * 8u;
    for (uint i = lidx; i < 256u; i += 64u)
        gSharedHist[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < Width && dtid.y < Height) {
        float3 c = gSource.Load(int3(dtid.xy, 0)).rgb;
        // Histogram luminance is expressed in the same 100-nit normalized
        // domain used by the gain-map/model path. DecodeToLinear keeps the
        // format-defined 80-nit scRGB conversion explicit at the boundary.
        float3 lin = DecodeToLinear(c, (int)(kFormat + 0.5),
                                    kSdrReferenceWhiteNits);
        float3 luma_weights = ((int)(kFormat + 0.5) == 2)
                                  ? kLuma2020
                                  : kLuma709;
        float luma = dot(max(lin, 0.0), luma_weights);

        float contribution = 1.0;
        if (kUseMask > 0.5) {
            uint2 mask_size = uint2(max(kMaskWidth, 1.0), max(kMaskHeight, 1.0));
            uint2 mask_xy = min(
                uint2((float2(dtid.xy) + 0.5) *
                      (float2(mask_size) / float2(Width, Height))),
                mask_size - 1u);
            float mask_value = saturate(gMask.Load(int3(mask_xy, 0)));
            contribution = round(mask_value * 16.0);
        }

        if (contribution >= 0.5) {
            float l2 = log2(max(luma, 1e-8));
            float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
            float t = saturate((l2 - kLumaMinLog2) / span);
            uint bin = min((uint)(t * 255.0 + 0.5), 255u);
            InterlockedAdd(gSharedHist[bin], (uint)contribution);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint hist_idx = lidx; hist_idx < 256u; hist_idx += 64u) {
        if (gSharedHist[hist_idx] > 0u)
            InterlockedAdd(gHistogram[hist_idx + (uint)(kHistogramOffset + 0.5)], gSharedHist[hist_idx]);
    }
}

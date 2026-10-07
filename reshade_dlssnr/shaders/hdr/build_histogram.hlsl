#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"

#define kFormat            shader.Params0.x
#define kLumaMinLog2       shader.Params0.y
#define kLumaMaxLog2       shader.Params0.z
#define kHasDepth          shader.Params1.x
#define kDepthInverted     shader.Params1.y
#define kHasMask           shader.Params1.z

Texture2D<float4> gSource   : register(t0);
Texture2D<float4> gDepth    : register(t1);
Texture2D<float4> gSkinMask : register(t2);
RWBuffer<uint>    gHistogram : register(u0);
SamplerState      s0 : register(s0);

static const uint kHistogramStride = 256u;


bool IsSkin(float3 lin)
{
    bool result = false;
    float3 p = pow(max(lin, 1e-6), 1.0 / 2.2);
    float mx = max(p.r, max(p.g, p.b));
    float mn = min(p.r, min(p.g, p.b));
    float d = mx - mn;
    if (d >= 1e-5 && mx >= 1e-5 && d / mx >= 0.15)
    {
        float h = 0.0;
        if (mx == p.r) h = 60.0 * ((p.g - p.b) / d);
        else if (mx == p.g) h = 60.0 * ((p.b - p.r) / d + 2.0);
        else h = 60.0 * ((p.r - p.g) / d + 4.0);
        if (h < 0.0) h += 360.0;
        result = h >= 15.0 && h <= 45.0;
    }
    return result;
}

bool IsSky(float d)
{
    return kDepthInverted > 0.5 ? (d < 0.05) : (d > 0.95);
}

groupshared uint gSharedHist[768];

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID,
            uint3 gtid : SV_GroupThreadID,
            uint3 dtid : SV_DispatchThreadID)
{
    const uint lidx = gtid.x + gtid.y * 8u;
    for (uint i = lidx; i < 768u; i += 64u)
        gSharedHist[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < Width && dtid.y < Height) {
        float3 c = gSource.Load(int3(dtid.xy, 0)).rgb;
        float3 lin = DecodeToLinear(c, (int)(kFormat + 0.5), kScRgbReferenceWhiteNits);
        float3 luma_weights = ((int)(kFormat + 0.5) == 2)
                                  ? kLuma2020
                                  : kLuma709;
        float luma = dot(max(lin, 0.0), luma_weights);

        float l2 = log2(max(luma, 1e-8));
        float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
        float t = saturate((l2 - kLumaMinLog2) / span);
        uint bin = min((uint)(t * 255.0 + 0.5), 255u);

        InterlockedAdd(gSharedHist[bin], 1u);

        float2 uv = (float2(dtid.xy) + 0.5) / float2(Width, Height);
        bool sky = false;
        if (kHasDepth > 0.5) {
            float d = gDepth.SampleLevel(s0, uv, 0).r;
            sky = IsSky(d);
        }
        if (!sky) {
            InterlockedAdd(gSharedHist[kHistogramStride + bin], 1u);
        }

        bool is_skin;
        if (kHasMask > 0.5) {
            is_skin = gSkinMask.SampleLevel(s0, uv, 0).r > 0.5;
        } else {
            is_skin = IsSkin(lin);
        }
        if (is_skin) {
            InterlockedAdd(gSharedHist[2u * kHistogramStride + bin], 1u);
        }
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint hist_idx = lidx; hist_idx < 768u; hist_idx += 64u) {
        if (gSharedHist[hist_idx] > 0u)
            InterlockedAdd(gHistogram[hist_idx], gSharedHist[hist_idx]);
    }
}

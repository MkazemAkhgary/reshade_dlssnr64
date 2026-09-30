// HDR shaders. Histogram building and the tonemap that normalizes an HDR
// source to the SDR image the model reads.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kZeroHistogramSource = R"(
RWBuffer<uint> gHistogram : register(u0);

[numthreads(768, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 768u)
        gHistogram[id.x] = 0u;
}
)";

inline constexpr const char* kBuildHistogramSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = format (0 SDR sRGB, 1 scRGB linear, 2 HDR10 PQ)
// gParams[0].y = luma min log2
// gParams[0].z = luma max log2
// gParams[1].x = depth available
// gParams[1].y = depth inverted
// gParams[1].z = skin mask bound
#define kFormat         gParams[0].x
#define kLumaMinLog2    gParams[0].y
#define kLumaMaxLog2    gParams[0].z
#define kHasDepth       gParams[1].x
#define kDepthInverted  gParams[1].y
#define kHasMask        gParams[1].z

Texture2D<float4> gSource   : register(t0);
Texture2D<float4> gDepth    : register(t1);
Texture2D<float4> gSkinMask : register(t2);
RWBuffer<uint>    gHistogram : register(u0);
SamplerState      s0 : register(s0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);
static const uint kHistogramStride = 256u;

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
}

float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) return max(c, 0.0);
    if (fmt == 2) return PqToNits(c) / 80.0;
    return SrgbToLinear(c);
}

bool IsSkin(float3 lin)
{
    float3 p = pow(max(lin, 1e-6), 1.0 / 2.2);
    float mx = max(p.r, max(p.g, p.b));
    float mn = min(p.r, min(p.g, p.b));
    float d = mx - mn;
    if (d < 1e-5 || mx < 1e-5) return false;
    if (d / mx < 0.15) return false;

    float h;
    if (mx == p.r) h = 60.0 * ((p.g - p.b) / d);
    else if (mx == p.g) h = 60.0 * ((p.b - p.r) / d + 2.0);
    else h = 60.0 * ((p.r - p.g) / d + 4.0);
    if (h < 0.0) h += 360.0;
    return h >= 15.0 && h <= 45.0;
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

    if (dtid.x < gWidth && dtid.y < gHeight) {
        float3 c = gSource.Load(int3(dtid.xy, 0)).rgb;
        float3 lin = DecodeToLinear(c);
        float luma = dot(max(lin, 0.0), kLuma709);

        float l2 = log2(max(luma, 1e-8));
        float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
        float t = saturate((l2 - kLumaMinLog2) / span);
        uint bin = min((uint)(t * 255.0 + 0.5), 255u);

        InterlockedAdd(gSharedHist[bin], 1u);

        float2 uv = (float2(dtid.xy) + 0.5) / float2(gWidth, gHeight);
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

    for (uint i = lidx; i < 768u; i += 64u) {
        if (gSharedHist[i] > 0u)
            InterlockedAdd(gHistogram[i], gSharedHist[i]);
    }
}
)";

inline constexpr const char* kTonemapForwardSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = exposure
// gParams[0].y = white point (Extended Reinhard W)
// gParams[0].z = format (0 SDR sRGB, 1 scRGB linear, 2 HDR10 PQ)
#define kExposure   gParams[0].x
#define kWhitePoint gParams[0].y
#define kFormat     gParams[0].z

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}

float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
}

float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) return max(c, 0.0);
    if (fmt == 2) return max(PqToNits(c), 0.0) / 80.0;
    return SrgbToLinear(c);
}

// HLSL truncates a float3 argument to .x when a parameter is scalar.
float3 ReinhardForward(float3 x, float W)
{
    float W2 = max(W * W, 1e-6);
    return x * (1.0 + x / W2) / (1.0 + x);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 src = gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeToLinear(src.rgb);
    lin *= kExposure;

    float3 sdr = ReinhardForward(lin, kWhitePoint);
    float3 out_rgb = LinearToSrgb(sdr);

    gOutput[id.xy] = float4(out_rgb, src.a);
}
)";

}  // namespace reshade_dlssnr

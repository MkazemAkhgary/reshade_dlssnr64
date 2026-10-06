// HDR shaders. Histogram building plus the lossless gain-map split used to
// produce the SDR base and raw f32 log2 gain.
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

inline constexpr const char* kGainMapSplitSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// Shared HDR/gain-map region. These slots are also used by the HDR compose.
#define kFormat              gParams[8].x
#define kExposure            gParams[8].y
#define kBt2446K1            gParams[8].z
#define kBt2446K2            gParams[8].w
// k3 is retained as a CPU-side tuning/trace parameter, but the direct
// Method-C algebra below needs only k1, k2 and k4.
#define kBt2446K4            gParams[9].x
#define kCrosstalk           gParams[9].y
#define kBaseOffset          gParams[9].z
#define kAlternateOffset     gParams[9].w
#define kGainMinLog2         gParams[10].x
#define kGainMaxLog2         gParams[10].y
#define kEffectiveSceneWhite gParams[10].z

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

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

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92,
                pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1)
        return c;                               // scRGB: 1.0 == 80 nits; signed
    if (fmt == 2)
        return max(PqToNits(c), 0.0) / 80.0;  // normalize to 80-nit units
    return SrgbToLinear(c);
}

float3 PreDesaturate(float3 c, float a)
{
    // Reference matrix used by BT.2446C's gain-map splitter:
    // diag = 1 - 2a, off-diagonal = a. It preserves neutral greys and is
    // exactly inverted after the luma tone map.
    float d = 1.0 - 2.0 * a;
    float s = a * (c.r + c.g + c.b);
    return float3(d * c.r + s, d * c.g + s, d * c.b + s);
}

float3 InversePreDesaturate(float3 c, float a)
{
    float denom = max(1.0 - 3.0 * a, 1e-6);
    float off = -a / denom;
    float diag = 1.0 - 2.0 * off;
    float sum = c.r + c.g + c.b;
    return float3(diag * c.r + off * (sum - c.r),
                  diag * c.g + off * (sum - c.g),
                  diag * c.b + off * (sum - c.b));
}

float Bt2446C(float y_nits)
{
    // Method C, written in the parameterization used by the reference
    // implementation: Y_ip = k4 / k1 and a logarithmic shoulder above it.
    float y = max(y_nits, 0.0);
    float y_ip = kBt2446K4 / max(kBt2446K1, 1e-6);
    if (y < y_ip)
        return kBt2446K1 * y;
    return kBt2446K2 * log(max(y / y_ip, 1e-6)) + kBt2446K4;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 src = gSource.SampleLevel(s0, uv, 0);

    float3 hdr = DecodeToLinear(src.rgb);
    float3 pre_hdr = PreDesaturate(hdr, saturate(kCrosstalk));
    float y_hdr = max(dot(pre_hdr, kLuma709), 0.0);

    // The histogram/exposure tracker already reports scene white after the
    // exposure EMA. HighlightHeadroom scales that reference into the effective
    // HDR peak used by Method C, while the gain map keeps the original HDR
    // values untouched for reconstruction.
    float effective_peak_nits = max(100.0,
        80.0 * max(kEffectiveSceneWhite, 0.01));
    float curve_scale = 100.0 / effective_peak_nits;
    // Keep the explicit exposure in the uniform contract: scene white is the
    // EMA's exposure-adjusted quantity, so multiplying again would double it.

    float y_curve = y_hdr * 80.0 * curve_scale;
    float y_sdr_pct = Bt2446C(y_curve);
    float y_sdr = y_sdr_pct / 100.0;
    y_sdr = max(y_sdr, 0.0);

    // Raw ISO 21496-1 log2 gain. Store it directly as f32 in alpha; there is
    // deliberately no normalization, gamma, or integer quantization here.
    float gain = log2(max(y_hdr + kAlternateOffset, 1e-12) /
                      max(y_sdr + kBaseOffset, 1e-12));
    float gain_scale = exp2(gain);

    // Apply the exact gain-map formula in the pre-desaturated RGB space.
    // Because the crosstalk matrix preserves the all-ones vector, the scalar
    // offsets commute with the matrix and the inverse is algebraically exact.
    float3 pre_sdr = (pre_hdr + kAlternateOffset) / max(gain_scale, 1e-12)
                   - kBaseOffset;
    float3 sdr = saturate(InversePreDesaturate(pre_sdr, saturate(kCrosstalk)));

    // Keep the base in the ordinary linear SDR range. The raw f32 gain is
    // deliberately not clipped or quantized, so highlight information can
    // continue to ride in the gain channel even when the SDR rendition clips.
    // BuildGainMapStats reports those excursions for tuning exposure/headroom.
    gOutput[id.xy] = float4(sdr, gain);
}
)";

inline constexpr const char* kZeroGainMapStatsSource = R"(
RWBuffer<uint> gStats : register(u0);

[numthreads(2, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0u) gStats[0] = 0xffffffffu; // min sentinel in sortable order
    if (id.x == 1u) gStats[1] = 0u;          // max sentinel in sortable order
    if (id.x == 0u) gStats[2] = 0u;          // SDR clamp count
}
)";

inline constexpr const char* kBuildGainMapStatsSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

Texture2D<float4> gPacked : register(t0);
RWBuffer<uint>    gStats  : register(u0);

#define kGainMinLog2 gParams[10].x
#define kGainMaxLog2 gParams[10].y

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float4 packed = gPacked.Load(int3(id.xy, 0));
    float gain = packed.a;
    // NaN/Inf cannot represent a useful gain statistic. Count them as a
    // clipped sample instead of feeding an undefined ordering into the atomics.
    if (gain != gain || abs(gain) > 1e20) {
        InterlockedAdd(gStats[2], 1u);
        return;
    }
    uint gain_bits = asuint(gain);

    // Atomic min/max on positive sortable floats. The gain range includes
    // negatives, so store an order-preserving float transform in uint space.
    uint sortable = (gain_bits & 0x80000000u) != 0u
        ? ~gain_bits
        : (gain_bits ^ 0x80000000u);
    InterlockedMin(gStats[0], sortable);
    InterlockedMax(gStats[1], sortable);

    if (gain < kGainMinLog2 || gain > kGainMaxLog2 ||
        packed.r < 0.0 || packed.r > 1.0 ||
        packed.g < 0.0 || packed.g > 1.0 ||
        packed.b < 0.0 || packed.b > 1.0) {
        InterlockedAdd(gStats[2], 1u);
    }
}
)";

}  // namespace reshade_dlssnr

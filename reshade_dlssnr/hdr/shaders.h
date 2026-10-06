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
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);
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
        float3 luma_weights = ((int)(kFormat + 0.5) == 2)
                                  ? kLuma2020
                                  : kLuma709;
        float luma = dot(max(lin, 0.0), luma_weights);

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
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);

float3 HdrLumaWeights()
{
    int fmt = (int)(kFormat + 0.5);
    return fmt == 2 ? kLuma2020 : kLuma709;
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
    // The gain-map domain uses 1.0 = 100 nits, i.e. the SDR white reference.
    // scRGB is 1.0 = 80 nits, so it needs the 80/100 reference scaling here.
    if (fmt == 1)
        return c * (80.0 / 100.0);
    if (fmt == 2)
        return max(PqToNits(c), 0.0) / 100.0;
    return SrgbToLinear(c);
}

float3 PreDesaturate(float3 c, float a)
{
    // Reference crosstalk matrix:
    //   [1-2a, a,    a   ]
    //   [a,    1-2a, a   ]
    //   [a,    a,    1-2a]
    // The cross-channel term is therefore a * (sum - channel). Adding
    // a * sum directly would silently change the diagonal to 1-a, making
    // InversePreDesaturate no longer the inverse and producing hue shifts.
    float d = 1.0 - 2.0 * a;
    float sum = c.r + c.g + c.b;
    return float3(d * c.r + a * (sum - c.r),
                  d * c.g + a * (sum - c.g),
                  d * c.b + a * (sum - c.b));
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

// Keep the SDR base inside [0, 1] without changing its RGB ratios. A scalar
// gain map is capable of recording this luminance rescaling exactly, whereas
// per-channel clipping is not: clipping one channel changes chromaticity and
// that information cannot be recovered from the single luma gain. This is the
// gain-map-friendly form of gamut mapping: move the whole RGB triple along its
// chromaticity ray until it fits the SDR cube.
float3 GamutFitPreserveChroma(float3 c)
{
    // The luma-only BT.2446C path plus an exactly matched matrix inverse keeps
    // a valid non-negative HDR RGB triple non-negative. Clamp only tiny
    // negative round-off before determining the fit scale.
    c = max(c, 0.0);
    float max_channel = max(c.r, max(c.g, c.b));
    if (max_channel > 1.0)
        c /= max_channel;
    return c;
}

float Bt2446C(float y_nits)
{
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

    const float3 luma = HdrLumaWeights();
    float3 hdr = DecodeToLinear(src.rgb);
    float3 pre_hdr = PreDesaturate(hdr, saturate(kCrosstalk));

    // Exposure is a scene-adaptation control. It changes the image presented
    // to the SDR model, but never becomes part of the alternate HDR image or
    // its gain-map reconstruction.
    float exposure = max(kExposure, 1e-6);
    float3 pre_exposed = pre_hdr * exposure;
    float y_exposed = max(dot(pre_exposed, luma), 0.0);

    // kEffectiveSceneWhite is measured in the histogram's scRGB-like 80-nit
    // domain. Convert the resulting peak to nits, while this gain-map working
    // domain remains normalized to SDR white = 100 nits.
    float effective_peak_nits = max(
        100.0, 80.0 * max(kEffectiveSceneWhite, 0.01));
    float y_curve = y_exposed * 10000.0 / effective_peak_nits;
    float y_sdr_pct = Bt2446C(y_curve);
    float y_sdr = max(y_sdr_pct / 100.0, 0.0);

    // BT.2446C is used as a luma-only curve. Preserve chromaticity by scaling
    // the entire RGB triple by the luma ratio, then undo the reference
    // crosstalk matrix. This is the defining LumaGainMapSplitter architecture:
    // a single scalar gain map must not be used to solve RGB channels
    // independently with a constant per-channel offset, because those offsets
    // pull dark chromatic pixels toward neutral grey and make highlights pale.
    float chroma_scale = y_exposed > 1e-8 ? y_sdr / y_exposed : 0.0;
    float3 pre_sdr = pre_exposed * chroma_scale;
    float3 sdr =
        InversePreDesaturate(pre_sdr, saturate(kCrosstalk));

    // Never hard-clip R/G/B independently here. At elevated exposure the
    // luma-only BT.2446C output can legitimately push one or more channels
    // above 1.0. Independent saturate() would alter R:G:B ratios, and the
    // one-channel gain map could not restore that lost chromaticity. Fit the
    // whole triple to the SDR gamut instead, so the gain map can carry the
    // resulting luminance change exactly.
    sdr = GamutFitPreserveChroma(sdr);

    // The gain is derived from the actual linear HDR and SDR lumas, not from
    // the temporary exposure-adjusted HDR sample. Applying this scalar gain to
    // the SDR luma reconstructs the original HDR luma exactly (within float
    // precision) when the SDR base was not clipped.
    float y_hdr_gain = max(dot(max(hdr, 0.0), luma), 0.0);
    float y_sdr_gain = max(dot(sdr, luma), 0.0);
    float gain = log2(
        max(y_hdr_gain + kAlternateOffset, 1e-12) /
        max(y_sdr_gain + kBaseOffset, 1e-12));

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
    if (id.x == 0u) gStats[2] = 0u;          // SDR gamut-fit count
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

#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gComposed : register(t0);
Texture2D<float4> gSource   : register(t1);
Texture2D<float4> gModelOut : register(t2);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

#define gLowInput gSource
#define gLowEnhanced gModelOut
#include "common/fsr.hlsli"
#undef gLowInput
#undef gLowEnhanced

#define kDiagMode shader.Params0.x
#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten hdr.LocalExposure.y
#define kLocalMaxDarken hdr.LocalExposure.z
#define kLocalExposureMidtone hdr.LocalExposure.w

float SampleSourceLuma(float2 uv)
{
    float3 c = gSource.SampleLevel(s0, saturate(uv), 0.0).rgb;
    float3 lin = DecodeToLinear(c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre = HdrPreDesaturate(lin, saturate(kCrosstalk));
    return max(dot(max(pre, 0.0), HdrLumaWeights()), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2, float2 texel)
{
    const float min_dim = min((float)Width, (float)Height);
    const float radius = max(1.0, min_dim * 0.01);
    const float2 r = texel * radius;

    float weighted = center_log2 * 4.0;
    float weight = 4.0;

    float l = log2(SampleSourceLuma(uv + float2(-r.x, 0.0)));
    float w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleSourceLuma(uv + float2(r.x, 0.0)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleSourceLuma(uv + float2(0.0, -r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleSourceLuma(uv + float2(0.0, r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;

    return weighted / max(weight, 1e-6);
}

float LocalExposureFactor(float2 uv, float raw_luma)
{
    if (kLocalExposureStrength <= 0.0 || raw_luma <= 1e-8)
        return 1.0;

    const float2 texel = 1.0 / float2(max(Width, 1u), max(Height, 1u));
    const float center_log2 = log2(raw_luma);
    const float local_base_log2 = LocalBaseLogLuma(uv, center_log2, texel);
    const float target_base_log2 =
        log2(max(kLocalExposureMidtone, 1e-6)) -
        log2(max(kExposure, 1e-6));
    const float adjusted_base_log2 = lerp(
        local_base_log2, target_base_log2,
        saturate(kLocalExposureStrength));
    const float stops = clamp(
        adjusted_base_log2 - local_base_log2,
        -kLocalMaxDarken, kLocalMaxBrighten);
    return exp2(stops);
}

float3 EncodeDiagnosticSdr(float3 sdr)
{
    int fmt = (int)(kFormat + 0.5);
    float3 v = max(sdr, 0.0);
    if (fmt == 1)
        return v * (kSdrReferenceWhiteNits / kScRgbReferenceWhiteNits);
    if (fmt == 2)
        return NitsToPq(v * kSdrReferenceWhiteNits);
    return v;
}

float3 ForwardHdrToModelSdr(float3 hdr_encoded, float2 uv)
{
    float3 hdr_linear = DecodeToLinear(
        hdr_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));
    float raw_luma = max(dot(max(pre_hdr, 0.0), HdrLumaWeights()), 0.0);
    float local_exposure = LocalExposureFactor(uv, raw_luma);
    float3 pre_exposed = pre_hdr * max(kExposure, 1e-6) * local_exposure;
    float y_exposed = max(dot(pre_exposed, HdrLumaWeights()), 0.0);

    float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    float y_sdr_pct = HdrBt2446Forward(y_curve);
    float y_sdr = max(y_sdr_pct / 100.0, 0.0);
    float chroma_scale = y_exposed > 1e-8 ? y_sdr / y_exposed : 0.0;
    float3 pre_sdr = pre_exposed * chroma_scale;
    float3 sdr = HdrInversePreDesaturate(pre_sdr, saturate(kCrosstalk));
    return LinearToSrgb(GamutFitPreserveChroma(sdr));
}

// The diagnostic intentionally disables RCAS/grade effects by comparing the
// raw model images against the HDR round-trip. Use the UI with grade input,
// grade output, and RCAS disabled when collecting a clean math test.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    float3 roundtrip = ForwardHdrToModelSdr(
        gComposed.SampleLevel(s0, uv, 0.0).rgb, uv);
    const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);
    const float2 easu_con3 = float2(0.0, 4.0 * kEasuCon1.y);
    float3 model_in = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                               easu_con3, 0);
    float3 model_out = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                                easu_con3, 1);

    int mode = (int)(kDiagMode + 0.5);
    float3 display = 0.0;
    if (mode == 0) {
        // 2x2 diagnostic: input / output / HDR->SDR roundtrip / error.
        bool right = uv.x >= 0.5;
        bool bottom = uv.y >= 0.5;
        if (!bottom && !right)
            display = model_in;
        else if (!bottom && right)
            display = model_out;
        else if (bottom && !right)
            display = roundtrip;
        else {
            float3 diff = roundtrip - model_out;
            display = saturate(0.5 + diff * 4.0);
        }
    } else if (mode == 1) {
        float3 diff = roundtrip - model_out;
        display = saturate(0.5 + diff * 4.0);
    } else if (mode == 2) {
        float3 src = gSource.SampleLevel(s0, uv, 0.0).rgb;
        float3 lin = DecodeToLinear(src, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
        float3 pre = HdrPreDesaturate(lin, saturate(kCrosstalk));
        float raw_luma = max(dot(max(pre, 0.0), HdrLumaWeights()), 0.0);
        float local = LocalExposureFactor(uv, raw_luma);
        float stops = log2(max(local, 1e-6));
        display = saturate(0.5 + stops / 4.0);
        display = display.xxx;
    }

    gOutput[id.xy] = float4(EncodeDiagnosticSdr(display), 1.0);
}

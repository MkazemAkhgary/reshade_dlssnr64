#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/box_filter.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);


#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten      hdr.LocalExposure.y
#define kLocalMaxDarken        hdr.LocalExposure.z
#define kLocalExposureMidtone  hdr.LocalExposure.w

float SampleHdrLuma(float2 uv)
{
    float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 lin = DecodeToLinear(c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    lin = HdrPreDesaturate(lin, saturate(kCrosstalk));
    return max(dot(max(lin, 0.0), HdrLumaWeights()), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2, float2 texel)
{
    // Approximate UE-style local exposure: estimate a low-frequency base
    // luminance with a small bilateral neighborhood, then adjust only that
    // base. The center/detail component is preserved.
    const float min_dim = min((float)Width, (float)Height);
    const float radius = max(1.0, min_dim * 0.01);
    const float2 r = texel * radius;

    float weighted = center_log2 * 4.0;
    float weight = 4.0;

    float l = log2(SampleHdrLuma(uv + float2(-r.x, 0.0)));
    float w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleHdrLuma(uv + float2(r.x, 0.0)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleHdrLuma(uv + float2(0.0, -r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;
    l = log2(SampleHdrLuma(uv + float2(0.0, r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w; weight += w;

    return weighted / max(weight, 1e-6);
}

float LocalExposureFactor(float2 uv, float raw_luma, float exposure)
{
    if (kLocalExposureStrength <= 0.0 || raw_luma <= 1e-8)
        return 1.0;

    const float2 texel = 1.0 / float2(max(Width, 1u), max(Height, 1u));
    const float center_log2 = log2(raw_luma);
    const float local_base_log2 = LocalBaseLogLuma(uv, center_log2, texel);
    const float target_base_log2 =
        log2(max(kLocalExposureMidtone, 1e-6)) - log2(max(exposure, 1e-6));
    const float adjusted_base_log2 = lerp(
        local_base_log2, target_base_log2,
        saturate(kLocalExposureStrength));
    const float stops = clamp(
        adjusted_base_log2 - local_base_log2,
        -kLocalMaxDarken, kLocalMaxBrighten);
    return exp2(stops);
}


[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    // When the split itself is evaluated below the source resolution (the skin
    // detector path), use the same exact footprint box as the main SDR model
    // input path. At 1:1 resolution this collapses to a single source texel.
    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    float4 src = BoxAreaAverage(gSource, id.xy, uint2(Width, Height),
                                uint2(src_width, src_height));

    const float3 luma = HdrLumaWeights();
    float3 hdr_linear = DecodeToLinear(
        src.rgb, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));

    // Exposure is a scene-adaptation control. It changes the image presented
    // to the SDR model, but never becomes part of the alternate HDR image or
    // its gain-map reconstruction.
    float exposure = max(kExposure, 1e-6);
    float raw_luma = max(dot(max(pre_hdr, 0.0), luma), 0.0);
    float local_exposure = LocalExposureFactor(
        (float2(id.xy) + 0.5) / float2(max(Width, 1u), max(Height, 1u)),
        raw_luma, exposure);
    float3 pre_exposed = pre_hdr * exposure * local_exposure;
    float y_exposed = max(dot(pre_exposed, luma), 0.0);

    // The gain-map working domain is normalized to SDR white = 100 nits.
    // The CPU has already converted the measured 80-nit scRGB-domain scene
    // white into absolute nits, so there is one explicit domain crossing.
    float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    float y_sdr_pct = HdrBt2446Forward(y_curve);
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
        HdrInversePreDesaturate(pre_sdr, saturate(kCrosstalk));

    // The packed RGB is the lossless linear SDR carrier. Do NOT gamut-fit or
    // clamp it here. At elevated exposure, BT.2446C can legitimately produce
    // super-white SDR values, and those values are part of the reversible
    // HDR->(SDR, gain) split. The model-facing path applies a hue-preserving
    // gamut fit later, after this carrier and its gain have been stored.

    // The gain is derived from the actual linear HDR and lossless SDR carrier
    // lumas, not from the temporary exposure-adjusted HDR sample.
    float y_hdr_gain = max(dot(max(hdr_linear, 0.0), luma), 0.0);
    float y_sdr_gain = max(dot(sdr, luma), 0.0);
    float gain = log2(
        max(y_hdr_gain + kAlternateOffset, 1e-12) /
        max(y_sdr_gain + kBaseOffset, 1e-12));

    gOutput[id.xy] = float4(sdr, gain);
}

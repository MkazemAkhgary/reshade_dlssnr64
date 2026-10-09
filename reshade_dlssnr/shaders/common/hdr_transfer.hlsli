#pragma once
#ifndef DLSSNR_HDR_TRANSFER_HLSLI
#define DLSSNR_HDR_TRANSFER_HLSLI

#include "bindings.hlsli"
#include "color.hlsli"
#include "luma.hlsli"


float3 HdrPreDesaturate(float3 c, float a)
{
    // Symmetric crosstalk matrix. Row sums remain 1, so this is a chroma
    // adjustment rather than an exposure change.
    float d = 1.0 - 2.0 * a;
    float sum = c.r + c.g + c.b;
    return float3(d * c.r + a * (sum - c.r),
                  d * c.g + a * (sum - c.g),
                  d * c.b + a * (sum - c.b));
}

float HdrBt2446Forward(float y_nits)
{
    float y = max(y_nits, 0.0);
    float y_ip = kBt2446K4 / max(kBt2446K1, 1e-6);
    float result = kBt2446K1 * y;
    if (y >= y_ip)
        result = kBt2446K2 * log(max(y / y_ip, 1e-6)) + kBt2446K4;
    return result;
}

float HdrBt2446Inverse(float sdr_pct)
{
    float y = max(sdr_pct, 0.0);
    float y_ip = kBt2446K4 / max(kBt2446K1, 1e-6);
    if (y <= kBt2446K4)
        return y / max(kBt2446K1, 1e-6);
    return y_ip * exp((y - kBt2446K4) / max(kBt2446K2, 1e-6));
}


// Reconstructs HDR from the SDR carrier for diagnostics. Alpha is the direct
// carrier-to-HDR multiplier stored in RGBA32F; no log/exp round trip, additive
// offsets, or fixed super-white ceiling is involved.
float3 HdrReconstructFromGainMap(float3 sdr_base, float carrier_to_hdr_scale)
{
    return max(sdr_base, 0.0) * max(carrier_to_hdr_scale, 0.0);
}

// Forward part of GainMapSplit before its local-exposure multiplier. Inverse
// Tone Map uses this with the untouched source to recover the exact local scale
// already embedded in the packed carrier, rather than re-running its estimator.
float3 HdrToGlobalLinearSdrCarrier(float3 hdr_linear, float exposure,
                                    float effective_peak_nits)
{
    const float3 luma = HdrLumaWeights();
    const float3 pre_hdr = HdrPreDesaturate(max(hdr_linear, 0.0), saturate(kCrosstalk));
    const float y_hdr = max(dot(pre_hdr, luma), 0.0);
    const float y_exposed = y_hdr * max(exposure, 1e-6);
    const float y_curve = y_exposed * kPqPeakNits /
        max(effective_peak_nits, kSdrReferenceWhiteNits);
    const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    const float chroma_scale = y_exposed > 0.0 ? y_sdr / y_exposed : 0.0;
    // Pre-desaturation and its inverse cancel algebraically; keep the resulting
    // RGB as a source-proportional scalar to avoid saturated-color cancellation.
    return max(hdr_linear, 0.0) * (max(exposure, 1e-6) * chroma_scale);
}

// Converts the normalized linear HDR working domain back into the host's
// encoded format. All math before this boundary stays in float shader values.
float3 HdrEncodeTarget(float3 linear_hdr, int format_code,
                       float reference_nits)
{
    const float3 positive = max(linear_hdr, 0.0);
    if (format_code == 1)
        return positive * (reference_nits / kScRgbReferenceWhiteNits);
    if (format_code == 2)
        return NitsToPq(positive * reference_nits);
    return positive;
}

float3 HdrFromLinearSdrCarrier(float3 sdr_linear, float exposure,
                               float effective_peak_nits)
{
    const float3 luma = HdrLumaWeights();
    float3 pre_sdr = HdrPreDesaturate(max(sdr_linear, 0.0), saturate(kCrosstalk));
    float y_sdr = max(dot(pre_sdr, luma), 0.0);
    if (y_sdr <= 0.0)
        return float3(0.0, 0.0, 0.0);

    const float y_curve = HdrBt2446Inverse(y_sdr * 100.0);
    const float y_exposed = y_curve *
                            max(effective_peak_nits, kSdrReferenceWhiteNits) /
                            kPqPeakNits;
    const float scale = y_exposed / y_sdr / max(exposure, 1e-6);
    // Invert the scalar exposure/tone map directly. The crosstalk transform
    // only participates in the luma curve and cancels out of the RGB signal.
    return max(sdr_linear, 0.0) * scale;
}

// Inverse of the model-facing SDR transform used by GainMapSplit. The input
// includes the reversible model-gamut transform but not the local SDR scale.
float3 HdrFromSdrCarrier(float3 sdr, float exposure,
                         float effective_peak_nits)
{
    const float3 model_linear = GamutUnfitModelInput(max(sdr, 0.0));
    return HdrFromLinearSdrCarrier(model_linear, exposure, effective_peak_nits);
}

#endif

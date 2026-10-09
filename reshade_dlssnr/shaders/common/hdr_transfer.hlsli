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

float3 HdrInversePreDesaturate(float3 c, float a)
{
    float denom = max(1.0 - 3.0 * a, 1e-6);
    float off = -a / denom;
    float diag = 1.0 - 2.0 * off;
    float sum = c.r + c.g + c.b;
    return float3(diag * c.r + off * (sum - c.r),
                  diag * c.g + off * (sum - c.g),
                  diag * c.b + off * (sum - c.b));
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

float3 HdrFromLinearSdrCarrier(float3 sdr_linear, float exposure,
                               float effective_peak_nits)
{
    const float3 luma = HdrLumaWeights();
    float3 pre_sdr = HdrPreDesaturate(max(sdr_linear, 0.0), saturate(kCrosstalk));
    float y_sdr = max(dot(pre_sdr, luma), 0.0);
    if (y_sdr <= 1e-8)
        return float3(0.0, 0.0, 0.0);

    const float y_curve = HdrBt2446Inverse(y_sdr * 100.0);
    const float y_exposed = y_curve *
                            max(effective_peak_nits, kSdrReferenceWhiteNits) /
                            kPqPeakNits;
    const float scale = y_exposed / y_sdr;
    const float3 pre_exposed = pre_sdr * scale;
    const float3 pre_hdr = pre_exposed / max(exposure, 1e-6);
    return max(HdrInversePreDesaturate(pre_hdr, saturate(kCrosstalk)), 0.0);
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

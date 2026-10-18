#pragma once

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

// BT.2446C tone curve, C1-continuous and invertible. k2 and k4 are
// derived from k1, k3, and the HDR inflection point so the two branches
// meet with matching value and slope at the knee. Larger k3 pulls the log
// branch down harder, compressing highlights more.
float HdrBt2446Forward(float y_nits)
{
    float y = max(y_nits, 0.0);
    float ip = max(kBt2446Ip, 1e-6);
    float k1 = max(kBt2446K1, 1e-6);
    float k3 = saturate(kBt2446K3);
    float k2 = k1 * ip * (1.0 - k3);
    float k4 = k1 * ip * (1.0 - (1.0 - k3) * log(1.0 - k3));
    float result = k1 * y;
    if (y >= ip)
        result = k2 * log(max(y / ip - k3, 1e-6)) + k4;
    return result;
}

// Exact inverse of HdrBt2446Forward. Both branches are strictly monotonic
// and meet at y_sdr = k1 * ip.
float HdrBt2446Inverse(float y_sdr)
{
    float ip = max(kBt2446Ip, 1e-6);
    float k1 = max(kBt2446K1, 1e-6);
    float k3 = saturate(kBt2446K3);
    float k2 = k1 * ip * (1.0 - k3);
    float k4 = k1 * ip * (1.0 - (1.0 - k3) * log(1.0 - k3));
    if (y_sdr < k1 * ip) return y_sdr / k1;
    return ip * (exp((y_sdr - k4) / max(k2, 1e-6)) + k3);
}

// Reconstructs HDR from the SDR carrier for diagnostics. Alpha is the direct
// carrier-to-HDR multiplier stored in RGBA32F; no log/exp round trip, additive
// offsets, or fixed super-white ceiling is involved.
float3 HdrReconstructFromGainMap(float3 sdr_base, float carrier_to_hdr_scale)
{
    return max(sdr_base, 0.0) * max(carrier_to_hdr_scale, 0.0);
}

// Converts the normalized linear HDR working domain back into the host's
// encoded format. All math before this boundary stays in float shader values.
float3 HdrEncodeTarget(float3 linear_hdr, int format_code,
                       float reference_nits)
{
    const float3 positive = max(linear_hdr, 0.0);
    float3 result = positive;
    if (format_code == 1)
        result = positive * (reference_nits / kScRgbReferenceWhiteNits);
    else if (format_code == 2)
        result = NitsToPq(positive * reference_nits);
    return result;
}

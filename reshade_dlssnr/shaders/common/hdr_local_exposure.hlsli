#pragma once

#include "bindings.hlsli"
#include "color.hlsli"
#include "luma.hlsli"
#include "hdr_transfer.hlsli"

#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten      hdr.LocalExposure.y
#define kLocalMaxDarken        hdr.LocalExposure.z
#define kLocalExposureMidtone  hdr.LocalExposure.w

Texture2D<float> gPrecomputedLuma : register(t3);

#define kLocalBlockSizePx 4.0
#define kLocalSampleCount 4u
#define kUsePrecomputedLocalLuma (hdr.LocalExposureParams.z > 0.5)

float GlobalHdrLumaToSdr(float hdr_luma, float exposure)
{
    const float y_exposed = max(hdr_luma, 0.0) * max(exposure, 1e-6);
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    return max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
}

float SampleGlobalSdrLuma(float2 uv)
{
    if (kUsePrecomputedLocalLuma)
        return max(gPrecomputedLuma.SampleLevel(s0, saturate(uv), 0), 1e-8);
    const float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 linear_rgb = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    linear_rgb = HdrPreDesaturate(linear_rgb, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(linear_rgb, 0.0), HdrLumaWeights()), 0.0);
    return max(GlobalHdrLumaToSdr(hdr_luma, kExposure), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2)
{
    uint source_width, source_height;
    gSource.GetDimensions(source_width, source_height);
    const float2 source_size = float2(max(source_width, 1u), max(source_height, 1u));
    const float min_dim = min(source_size.x, source_size.y);
    float radius_px = kLocalBlockSizePx * 0.5;
    float max_radius_px = min_dim * 0.25;
    max_radius_px = max(max_radius_px, 1.0);
    radius_px = clamp(radius_px, 1.0, max_radius_px);
    const float2 radius_uv = radius_px / source_size;

    // First 12 preserve the original low-cost cardinal/diagonal pattern. Three
    // additional concentric rings (r=0.30, 0.60, 0.90) fill gaps up to 48 taps.
    const float2 offsets[48] = {
        float2(-0.5000, 0.0000), float2(0.5000, 0.0000), float2(0.0000, -0.5000),
        float2(0.0000, 0.5000), float2(-1.0000, 0.0000), float2(1.0000, 0.0000),
        float2(0.0000, -1.0000), float2(0.0000, 1.0000), float2(-1.0000, -1.0000),
        float2(1.0000, -1.0000), float2(-1.0000, 1.0000), float2(1.0000, 1.0000),
        float2(0.2898, 0.0776), float2(0.2121, 0.2121), float2(0.0776, 0.2898),
        float2(-0.0776, 0.2898), float2(-0.2121, 0.2121), float2(-0.2898, 0.0776),
        float2(-0.2898, -0.0776), float2(-0.2121, -0.2121), float2(-0.0776, -0.2898),
        float2(0.0776, -0.2898), float2(0.2121, -0.2121), float2(0.2898, -0.0776),
        float2(0.5796, 0.1553), float2(0.4243, 0.4243), float2(0.1553, 0.5796),
        float2(-0.1553, 0.5796), float2(-0.4243, 0.4243), float2(-0.5796, 0.1553),
        float2(-0.5796, -0.1553), float2(-0.4243, -0.4243), float2(-0.1553, -0.5796),
        float2(0.1553, -0.5796), float2(0.4243, -0.4243), float2(0.5796, -0.1553),
        float2(0.8693, 0.2329), float2(0.6364, 0.6364), float2(0.2329, 0.8693),
        float2(-0.2329, 0.8693), float2(-0.6364, 0.6364), float2(-0.8693, 0.2329),
        float2(-0.8693, -0.2329), float2(-0.6364, -0.6364), float2(-0.2329, -0.8693),
        float2(0.2329, -0.8693), float2(0.6364, -0.6364), float2(0.8693, -0.2329)
    };

    // Keep the original spatial weights for the original 12 taps, then use
    // distance falloff for the additional rings. The extra taps improve coverage
    // without changing the legacy spatial balance at a sample count of 12.
    const float spatial_weights[12] = {
        1.25, 1.25, 1.25, 1.25,
        1.0, 1.0, 1.0, 1.0,
        0.75, 0.75, 0.75, 0.75};
    float weighted = center_log2 * 3.0;
    float weight = 3.0;
    uint sample_count = kLocalSampleCount;
    sample_count = min(sample_count, 48u);
    [loop]
    for (uint i = 0u; i < sample_count; ++i) {
        const float2 sample_uv = saturate(uv + offsets[i] * radius_uv);
        const float local_luma = log2(SampleGlobalSdrLuma(sample_uv));
        const float radius_sq = dot(offsets[i], offsets[i]);
        const float spatial_weight = i < 12u
            ? spatial_weights[i]
            : exp2(-1.5 * radius_sq);
        weighted += local_luma * spatial_weight;
        weight += spatial_weight;
    }
    return weighted / max(weight, 1e-6);
}

float LocalExposureScale(float2 uv, float global_sdr_luma)
{
    float result = 1.0;
    if (!(kLocalExposureStrength <= 0.0 || global_sdr_luma <= 1e-8))
    {
        const float center_log2 = log2(max(global_sdr_luma, 1e-8));
        const float base_log2 = LocalBaseLogLuma(uv, center_log2);
        const float detail_log2 = center_log2 - base_log2;
        float local_midtone = kLocalExposureMidtone;
        local_midtone = max(local_midtone, 1e-6);
        const float mid_log2 = log2(local_midtone);
        const float distance_from_mid = base_log2 - mid_log2;
        const float shadow_weight = 1.0 - smoothstep(-2.5, -0.5, distance_from_mid);
        const float highlight_weight = smoothstep(0.5, 2.5, distance_from_mid);
        const float region_weight = max(shadow_weight, highlight_weight);
        float strength = kLocalExposureStrength;
        strength = saturate(strength);
        strength *= region_weight;
        const float new_base_log2 = lerp(base_log2, mid_log2, strength);
        const float new_log2 = new_base_log2 + detail_log2;
        float stops = new_log2 - center_log2;
        const float min_stops = -kLocalMaxDarken;
        const float max_stops = kLocalMaxBrighten;
        stops = clamp(stops, min_stops, max_stops);
        result = exp2(stops);
    }
    return result;
}

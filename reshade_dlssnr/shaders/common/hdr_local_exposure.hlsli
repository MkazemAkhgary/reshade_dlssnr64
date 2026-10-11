#pragma once

#include "bindings.hlsli"
#include "color.hlsli"
#include "luma.hlsli"
#include "hdr_transfer.hlsli"

#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten      hdr.LocalExposure.y
#define kLocalMaxDarken        hdr.LocalExposure.z
#define kLocalExposureMidtone  hdr.LocalExposure.w

#define kLocalFilterFlags ((uint)(shader.Params0.x + 0.5))
#define kLocalDepthNear shader.Params0.y
#define kLocalDepthFar shader.Params0.z
#define kLocalDepthInverted shader.Params0.w
#define kLocalLumaMin shader.Params1.x
#define kLocalLumaMax shader.Params1.y
#define kLocalBlockSizePx shader.Params1.z
#define kLocalDepthEdgeThreshold shader.Params1.w
#define kLocalSampleCount ((kLocalFilterFlags >> 8u) & 0xFFu)
#define kLocalEdgeSharpness hdr.LocalExposureEdgeSharpness

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
    const float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 linear_rgb = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    linear_rgb = HdrPreDesaturate(linear_rgb, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(linear_rgb, 0.0), HdrLumaWeights()), 0.0);
    return max(GlobalHdrLumaToSdr(hdr_luma, kExposure), 1e-8);
}

float SampleLocalDepth(float2 uv)
{
    uint depth_width, depth_height;
    gDepth.GetDimensions(depth_width, depth_height);
    const uint2 depth_size = max(uint2(depth_width, depth_height), uint2(1u, 1u));
    const uint2 depth_xy = min(
        uint2(saturate(uv) * float2(depth_size)), depth_size - 1u);
    float depth = saturate(gDepth.Load(int3(depth_xy, 0)));
    if (kLocalDepthInverted > 0.5) depth = 1.0 - depth;
    return depth;
}

float LocalBaseLogLuma(float2 uv, float center_log2)
{
    uint source_width, source_height;
    gSource.GetDimensions(source_width, source_height);
    const float2 source_size = float2(max(source_width, 1u), max(source_height, 1u));
    const float min_dim = min(source_size.x, source_size.y);
    const float radius_px = clamp(kLocalBlockSizePx * 0.5, 1.0,
                                  max(min_dim * 0.25, 1.0));
    const float2 radius_uv = radius_px / source_size;
    const bool use_depth_continuity = (kLocalFilterFlags & 8u) != 0u;
    const float center_depth = use_depth_continuity ? SampleLocalDepth(uv) : 0.0;
    const float depth_threshold = max(kLocalDepthEdgeThreshold, 1e-5);

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
    const uint sample_count = min(kLocalSampleCount, 48u);
    const float edge_sharpness = max(kLocalEdgeSharpness, 0.0);
    [loop]
    for (uint i = 0u; i < sample_count; ++i) {
        const float2 sample_uv = saturate(uv + offsets[i] * radius_uv);
        const float local_luma = log2(SampleGlobalSdrLuma(sample_uv));
        const float luma_delta = abs(local_luma - center_log2);
        const float luminance_edge = exp2(-edge_sharpness * luma_delta);
        float depth_weight = 1.0;
        if (use_depth_continuity) {
            const float sample_depth = SampleLocalDepth(sample_uv);
            const float depth_delta = abs(sample_depth - center_depth);
            depth_weight = 1.0 - smoothstep(
                depth_threshold * 0.5, depth_threshold, depth_delta);
        }
        const float radius_sq = dot(offsets[i], offsets[i]);
        const float spatial_weight = i < 12u
            ? spatial_weights[i]
            : exp2(-1.5 * radius_sq);
        const float sample_weight = spatial_weight * luminance_edge * depth_weight;
        weighted += local_luma * sample_weight;
        weight += sample_weight;
    }
    return weighted / max(weight, 1e-6);
}

float LocalFilterWeight(float2 uv, float source_hdr_luma)
{
    float weight = 1.0;
    if ((kLocalFilterFlags & 1u) != 0u) {
        uint mask_width, mask_height;
        gMask.GetDimensions(mask_width, mask_height);
        const uint2 mask_size = max(uint2(mask_width, mask_height), uint2(1u, 1u));
        const uint2 mask_xy = min(
            uint2(saturate(uv) * float2(mask_size)), mask_size - 1u);
        weight *= saturate(gMask.Load(int3(mask_xy, 0)));
    }
    if ((kLocalFilterFlags & 2u) != 0u) {
        const float depth = SampleLocalDepth(uv);
        if (depth < kLocalDepthNear || depth > kLocalDepthFar)
            weight = 0.0;
    }
    if ((kLocalFilterFlags & 4u) != 0u) {
        const float lower_edge = max(kLocalLumaMin, 1e-8);
        const float upper_edge = max(kLocalLumaMax, lower_edge + 1e-6);
        const float lower_width = max(lower_edge * 0.08, 1e-6);
        const float upper_width = max(upper_edge * 0.08, 1e-6);
        const float low_weight = smoothstep(
            max(lower_edge - lower_width, 0.0), lower_edge + lower_width,
            max(source_hdr_luma, 0.0));
        const float high_weight = 1.0 - smoothstep(
            max(upper_edge - upper_width, lower_edge), upper_edge + upper_width,
            max(source_hdr_luma, 0.0));
        weight *= saturate(low_weight * high_weight);
    }
    return saturate(weight);
}

float LocalExposureScale(float2 uv, float global_sdr_luma,
                         float source_hdr_luma)
{
    float result = 1.0;
    if (!(kLocalExposureStrength <= 0.0 || global_sdr_luma <= 1e-8))
    {
        const float center_log2 = log2(max(global_sdr_luma, 1e-8));
        const float base_log2 = LocalBaseLogLuma(uv, center_log2);
        const float detail_log2 = center_log2 - base_log2;
        const float mid_log2 = log2(max(kLocalExposureMidtone, 1e-6));
        const float distance_from_mid = base_log2 - mid_log2;
        const float shadow_weight = 1.0 - smoothstep(-2.5, -0.5, distance_from_mid);
        const float highlight_weight = smoothstep(0.5, 2.5, distance_from_mid);
        const float region_weight = max(shadow_weight, highlight_weight);
        const float strength = saturate(kLocalExposureStrength) * region_weight;
        const float new_base_log2 = lerp(base_log2, mid_log2, strength);
        const float new_log2 = new_base_log2 + detail_log2;
        const float stops = clamp(new_log2 - center_log2,
                                  -kLocalMaxDarken, kLocalMaxBrighten);
        const float scale = exp2(stops);
        result = lerp(1.0, scale, LocalFilterWeight(uv, source_hdr_luma));
    }
    return result;
}

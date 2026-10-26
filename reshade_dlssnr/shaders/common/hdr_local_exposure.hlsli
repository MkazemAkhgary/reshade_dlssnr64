#pragma once

#include "bindings.hlsli"
#include "color.hlsli"
#include "luma.hlsli"
#include "hdr_transfer.hlsli"

#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten      hdr.LocalExposure.y
#define kLocalMaxDarken        hdr.LocalExposure.z
#define kLocalExposureMidtone  hdr.LocalExposure.w

#define kLocalRadiusPx max(hdr.LocalExposureParams.x, 1.0)
#define kLocalSampleCount ((uint)clamp(round(hdr.LocalExposureParams.y / 4.0) * 4.0, 4.0, 32.0))

float GlobalHdrLumaToSdr(float hdr_luma, float exposure)
{
    const float y_exposed = max(hdr_luma, 0.0) * max(exposure, 1e-6);
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    const float result = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    return result;
}

float SampleGlobalSdrLuma(float2 uv)
{
    const float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 linear_rgb = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    linear_rgb = HdrPreDesaturate(linear_rgb, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(linear_rgb, 0.0), HdrLumaWeights()), 0.0);
    const float result = max(GlobalHdrLumaToSdr(hdr_luma, kExposure), 1e-8);
    return result;
}

float LocalBaseLogLuma(float2 uv, float center_log2)
{
    float result = center_log2;
    if (kLocalRadiusPx > 1.0)
    {
        uint source_width, source_height;
        gSource.GetDimensions(source_width, source_height);
        const float2 source_size = float2(max(source_width, 1u), max(source_height, 1u));
        const float min_dim = min(source_size.x, source_size.y);
        const float max_radius_px = max(min_dim * 0.25, 1.0);
        const float radius_px = clamp(kLocalRadiusPx, 1.0, max_radius_px);
        const float2 radius_uv = radius_px / source_size;

        // First 12 preserve the original low-cost cardinal/diagonal pattern. Three
        // additional concentric rings (r=0.30, 0.60, 0.90) fill gaps up to 48 taps.
        const float2 offsets[48] = {
            float2(-0.5000, 0.0000), float2(0.5000, 0.0000), float2(0.0000, -0.5000),
            float2(0.0000, 0.5000), float2(-1.0000, 0.0000), float2(1.0000, 0.0000),
            float2(0.0000, -1.0000), float2(0.0000, 1.0000), float2(-1.0000, -1.0000),
            float2(1.0000, -1.0000), float2(-1.0000, 1.0000), float2(1.0000, 1.0000),
            float2(0.2898, 0.0776), float2(-0.0776, 0.2898), float2(-0.2898, -0.0776), float2(0.0776, -0.2898),
            float2(0.2121, 0.2121), float2(-0.2121, 0.2121), float2(-0.2121, -0.2121), float2(0.2121, -0.2121),
            float2(0.0776, 0.2898), float2(-0.2898, 0.0776), float2(-0.0776, -0.2898), float2(0.2898, -0.0776),
            float2(0.5796, 0.1553), float2(-0.1553, 0.5796), float2(-0.5796, -0.1553), float2(0.1553, -0.5796),
            float2(0.4243, 0.4243), float2(-0.4243, 0.4243), float2(-0.4243, -0.4243), float2(0.4243, -0.4243),
            float2(0.1553, 0.5796), float2(-0.5796, 0.1553), float2(-0.1553, -0.5796), float2(0.5796, -0.1553),
            float2(0.8693, 0.2329), float2(-0.2329, 0.8693), float2(-0.8693, -0.2329), float2(0.2329, -0.8693),
            float2(0.6364, 0.6364), float2(-0.6364, 0.6364), float2(-0.6364, -0.6364), float2(0.6364, -0.6364),
            float2(0.2329, 0.8693), float2(-0.8693, 0.2329), float2(-0.2329, -0.8693), float2(0.8693, -0.2329)
        };

        // Preserve the legacy weighting of the first three rings. Additional
        // samples use distance falloff to improve neighborhood coverage.
        float weighted = center_log2 * 3.0;
        float weight = 3.0;
        const uint sample_count = kLocalSampleCount;
        [loop]
        for (uint i = 0u; i < sample_count; ++i) {
            const float2 sample_uv = saturate(uv + offsets[i] * radius_uv);
            const float local_luma = log2(SampleGlobalSdrLuma(sample_uv));
            const float luminance_delta = abs(local_luma - center_log2);
            const float edge_protection = saturate(localExposure.Detail.w);
            const float edge_weight = exp2(
                -edge_protection * luminance_delta * luminance_delta);
            const float radius_sq = dot(offsets[i], offsets[i]);
            float spatial_weight;
            if (i < 4u)
                spatial_weight = 1.25;
            else if (i < 8u)
                spatial_weight = 1.0;
            else if (i < 12u)
                spatial_weight = 0.75;
            else
                spatial_weight = exp2(-1.5 * radius_sq);
            const float sample_weight = spatial_weight * edge_weight;
            weighted += local_luma * sample_weight;
            weight += sample_weight;
        }
        result = weighted / max(weight, 1e-6);
    }
    return result;
}

float LocalExposureScale(float2 uv, float global_sdr_luma)
{
    float result = 1.0;
    if (kLocalExposureStrength > 0.0 && global_sdr_luma > 1e-8) {
        const float center_log2 = log2(max(global_sdr_luma, 1e-8));
        const float mid_log2 = log2(max(kLocalExposureMidtone, 1e-6));
        if (abs(center_log2 - mid_log2) >= 1e-6) {
            const float base_log2 = LocalBaseLogLuma(uv, center_log2);
            const float detail_log2 = center_log2 - base_log2;
            const float distance_to_mid = mid_log2 - base_log2;

            const float shadow_range = max(localExposure.Shape.x, 0.25);
            const float highlight_range = max(localExposure.Shape.y, 0.25);
            const float highlight_protection = saturate(localExposure.Shape.z);
            const float shadow_protection = saturate(localExposure.Shape.w);
            const float detail_retention = saturate(localExposure.Detail.x);
            const float shadow_strength = saturate(localExposure.Detail.y);
            const float highlight_strength = saturate(localExposure.Detail.z);

            float base_shift = 0.0;
            if (distance_to_mid > 0.0)
            {
                const float transition = smoothstep(0.0, shadow_range, distance_to_mid);
                base_shift = distance_to_mid * transition *
                             saturate(kLocalExposureStrength) * shadow_strength;
            }
            else if (distance_to_mid < 0.0)
            {
                const float highlight_distance = -distance_to_mid;
                const float transition = smoothstep(0.0, highlight_range, highlight_distance);
                base_shift = -highlight_distance * transition *
                             saturate(kLocalExposureStrength) * highlight_strength;
            }

            // The local neighborhood selects the correction direction, but the pixel's
            // own luminance determines whether it should receive that correction. This
            // keeps the pivot stable, lifts shadow pixels without lifting bright spots,
            // and reduces highlights without crushing dark detail. The protection
            // sliders control how much correction is allowed to cross the pivot.
            const float pixel_from_mid = center_log2 - mid_log2;
            bool cross_pivot_correction = false;
            if (base_shift > 0.0)
            {
                if (pixel_from_mid < 0.0)
                {
                    const float pixel_gate =
                        smoothstep(0.0, shadow_range, -pixel_from_mid);
                    base_shift = min(base_shift * pixel_gate, -pixel_from_mid);
                }
                else
                {
                    cross_pivot_correction = highlight_protection < 1.0;
                    const float pixel_gate = (1.0 - highlight_protection) *
                        smoothstep(0.0, highlight_range, pixel_from_mid);
                    base_shift *= pixel_gate;
                }
            }
            else if (base_shift < 0.0)
            {
                if (pixel_from_mid > 0.0)
                {
                    const float pixel_gate =
                        smoothstep(0.0, highlight_range, pixel_from_mid);
                    base_shift = max(base_shift * pixel_gate, -pixel_from_mid);
                }
                else
                {
                    cross_pivot_correction = shadow_protection < 1.0;
                    const float pixel_gate = (1.0 - shadow_protection) *
                        smoothstep(0.0, shadow_range, -pixel_from_mid);
                    base_shift *= pixel_gate;
                }
            }

            const float new_base_log2 = base_log2 + base_shift;
            const float new_log2 = new_base_log2 + detail_log2 * detail_retention;
            float stops = clamp(new_log2 - center_log2,
                                -max(kLocalMaxDarken, 0.0),
                                max(kLocalMaxBrighten, 0.0));
            float adjusted_log2 = center_log2 + stops;

            // When cross-pivot correction is disabled, the pivot is also a hard output
            // shoulder/toe: strong settings cannot push a shadow above the pivot or a
            // highlight below it. This is what prevents Max Brighten = 4+ from blowing
            // out mid/dark details while still allowing deep shadows to receive a lift.
            if (!cross_pivot_correction)
            {
                if (pixel_from_mid < 0.0)
                    adjusted_log2 = min(adjusted_log2, mid_log2);
                else
                    adjusted_log2 = max(adjusted_log2, mid_log2);
            }
            result = exp2(adjusted_log2 - center_log2);
        }
    }
    return result;
}

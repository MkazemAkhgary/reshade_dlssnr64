// Shared SDR/HDR joint-bilateral reconstruction for model input/output.
#pragma once

float3 JointBilateralUpsample(
    int2 full_xy,
    int2 full_size,
    Texture2D<float4> low_tex,
    Texture2D<float4> guide_tex,
    float sharpness)
{
    uint low_w_u = 0u;
    uint low_h_u = 0u;
    low_tex.GetDimensions(low_w_u, low_h_u);
    const int low_w = max((int)low_w_u, 1);
    const int low_h = max((int)low_h_u, 1);
    if (full_size.x <= 0 || full_size.y <= 0) return float3(0.0, 0.0, 0.0);

    const float2 full_sz = float2((float)full_size.x, (float)full_size.y);
    const float2 low_sz  = float2((float)low_w, (float)low_h);

    // Pixel-center projection from full-res to low-res space. At 1:1 this
    // lands exactly on the source pixel; at other scales it interpolates.
    const float2 full_center = float2((float)full_xy.x + 0.5,
                                      (float)full_xy.y + 0.5);
    const float2 low_pix = full_center * (low_sz / full_sz);
    const int2 low_center = int2((int)floor(low_pix.x),
                                 (int)floor(low_pix.y));

    const float3 guide_center = guide_tex.Load(int3(full_xy, 0)).rgb;
    const float guide_luma_center =
        dot(guide_center, float3(0.2126, 0.7152, 0.0722));

    // Bilateral weighting. The spatial term is essential: with range-only
    // weighting every full-res pixel inside one low-res cell reads the same
    // 3x3 neighborhood with the same weights, so the cell collapses to a
    // constant and the output shows a fixed grid at low working scales.
    // Adding the distance from the continuous projected position low_pix
    // makes each full-res pixel's weights unique and continuous, so the
    // reconstruction varies smoothly across each cell and agrees at cell
    // boundaries.
    //
    // Kernel half-size from the pass's Upsample Kernel setting:
    //   3x3 -> half = 1, 5x5 -> half = 2, 7x7 -> half = 3
    //
    // Sigma is derived from the half-size, sigma = half. That puts the
    // spatial weight at the outermost diagonal tap at exp(-2) for every
    // kernel size, so the shape of the falloff is identical and only the
    // number of contributing taps changes. There is no separate sigma
    // uniform: the kernel size is the only control.
    const int kernel_half = max((int)(kUpsampleKernelHalf + 0.5), 1);
    const float spatial_sigma = (float)kernel_half;
    const float spatial_sigma_inv2 = 1.0 / (spatial_sigma * spatial_sigma);

    float3 accum = float3(0.0, 0.0, 0.0);
    float  weight_sum = 0.0;

    [loop]
    for (int dy = -kernel_half; dy <= kernel_half; ++dy) {
        [loop]
        for (int dx = -kernel_half; dx <= kernel_half; ++dx) {
            const int2 low_xy = int2(low_center.x + dx, low_center.y + dy);
            if (low_xy.x < 0 || low_xy.y < 0 ||
                low_xy.x >= low_w || low_xy.y >= low_h) {
                continue;
            }

            // Spatial distance from the continuous projected position to
            // this sample's center, in low-res pixel units.
            const float2 sample_center = float2((float)low_xy.x + 0.5,
                                                (float)low_xy.y + 0.5);
            const float2 offset = sample_center - low_pix;
            const float spatial_weight =
                exp(-dot(offset, offset) * spatial_sigma_inv2);

            // Full-res position of this low-res sample's center, so the
            // guide is read at the same feature the sample represents.
            const float2 low_neighbor_uv = sample_center / low_sz;
            int2 full_neighbor_xy = int2(
                (int)floor(low_neighbor_uv.x * full_sz.x),
                (int)floor(low_neighbor_uv.y * full_sz.y));
            full_neighbor_xy.x =
                clamp(full_neighbor_xy.x, 0, full_size.x - 1);
            full_neighbor_xy.y =
                clamp(full_neighbor_xy.y, 0, full_size.y - 1);

            const float3 guide_neighbor =
                guide_tex.Load(int3(full_neighbor_xy, 0)).rgb;
            const float guide_luma_neighbor =
                dot(guide_neighbor, float3(0.2126, 0.7152, 0.0722));

            const float luma_diff =
                abs(guide_luma_neighbor - guide_luma_center);
            const float range_weight = exp(-luma_diff * sharpness);

            const float weight = spatial_weight * range_weight;
            const float3 low_val = low_tex.Load(int3(low_xy, 0)).rgb;
            accum += low_val * weight;
            weight_sum += weight;
        }
    }

    if (weight_sum > 0.0) return accum / weight_sum;

    // Degenerate weight sum: fall back to the projected sample directly.
    const int2 clamped_center = int2(
        clamp(low_center.x, 0, low_w - 1),
        clamp(low_center.y, 0, low_h - 1));
    return low_tex.Load(int3(clamped_center, 0)).rgb;
}

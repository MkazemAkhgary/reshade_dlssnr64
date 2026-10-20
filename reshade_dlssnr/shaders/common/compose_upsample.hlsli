// Shared SDR/HDR joint-bilateral reconstruction for model input/output.
#pragma once

// Reconstruct input and enhanced output together. Coordinates, guide reads,
// luma differences and spatial weights are shared; range weights remain
// separate because the two callers intentionally use different sharpness.
void JointBilateralUpsamplePair(
    int2 full_xy,
    int2 full_size,
    Texture2D<float4> low_input,
    Texture2D<float4> low_output,
    Texture2D<float4> guide_tex,
    float input_sharpness,
    float output_sharpness,
    out float3 input_result,
    out float3 output_result)
{
    input_result = float3(0.0, 0.0, 0.0);
    output_result = float3(0.0, 0.0, 0.0);
    uint low_w_u = 0u;
    uint low_h_u = 0u;
    low_input.GetDimensions(low_w_u, low_h_u);
    const int low_w = max((int)low_w_u, 1);
    const int low_h = max((int)low_h_u, 1);
    const float2 full_sz = float2((float)full_size.x, (float)full_size.y);
    const float2 low_sz = float2((float)low_w, (float)low_h);
    const float2 full_center = float2((float)full_xy.x + 0.5,
                                      (float)full_xy.y + 0.5);
    const float2 low_pix = full_center * (low_sz / full_sz);
    const int2 low_center = int2((int)floor(low_pix.x), (int)floor(low_pix.y));
    const float3 guide_center = guide_tex.Load(int3(full_xy, 0)).rgb;
    const float guide_luma_center =
        dot(guide_center, float3(0.2126, 0.7152, 0.0722));
    const float spatial_sigma_inv2 = 1.0;

    float3 input_accum = float3(0.0, 0.0, 0.0);
    float3 output_accum = float3(0.0, 0.0, 0.0);
    float input_weight_sum = 0.0;
    float output_weight_sum = 0.0;
    [loop]
    for (int dy = -1; dy <= 1; ++dy) {
        [loop]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 low_xy = int2(low_center.x + dx, low_center.y + dy);
            if (low_xy.x < 0 || low_xy.y < 0 ||
                low_xy.x >= low_w || low_xy.y >= low_h)
                continue;

            const float2 sample_center = float2((float)low_xy.x + 0.5,
                                                (float)low_xy.y + 0.5);
            const float2 offset = sample_center - low_pix;
            const float spatial_weight =
                exp(-dot(offset, offset) * spatial_sigma_inv2);
            const float2 low_neighbor_uv = sample_center / low_sz;
            int2 full_neighbor_xy = int2(
                (int)floor(low_neighbor_uv.x * full_sz.x),
                (int)floor(low_neighbor_uv.y * full_sz.y));
            full_neighbor_xy.x = clamp(full_neighbor_xy.x, 0, full_size.x - 1);
            full_neighbor_xy.y = clamp(full_neighbor_xy.y, 0, full_size.y - 1);

            const float3 guide_neighbor =
                guide_tex.Load(int3(full_neighbor_xy, 0)).rgb;
            const float guide_luma_neighbor =
                dot(guide_neighbor, float3(0.2126, 0.7152, 0.0722));
            const float luma_diff = abs(guide_luma_neighbor - guide_luma_center);
            const float input_weight =
                spatial_weight * exp(-luma_diff * input_sharpness);
            const float output_weight =
                spatial_weight * exp(-luma_diff * output_sharpness);
            input_accum += low_input.Load(int3(low_xy, 0)).rgb * input_weight;
            output_accum += low_output.Load(int3(low_xy, 0)).rgb * output_weight;
            input_weight_sum += input_weight;
            output_weight_sum += output_weight;
        }
    }

    const int2 fallback_xy = int2(clamp(low_center.x, 0, low_w - 1),
                                  clamp(low_center.y, 0, low_h - 1));
    if (input_weight_sum > 0.0)
        input_result = input_accum / input_weight_sum;
    else
        input_result = low_input.Load(int3(fallback_xy, 0)).rgb;

    if (output_weight_sum > 0.0)
        output_result = output_accum / output_weight_sum;
    else
        output_result = low_output.Load(int3(fallback_xy, 0)).rgb;
}

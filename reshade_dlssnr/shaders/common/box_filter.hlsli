#pragma once

float4 BoxAreaAverage(Texture2D<float4> source,
                      uint2 dst_xy,
                      uint2 dst_size,
                      uint2 src_size)
{
    // Exact integer downscales have equal-area footprints. Handle the common
    // 1:1 through 4:1 ratios with fixed-load paths before the fractional path.
    if (dst_size.x > 0u && dst_size.y > 0u &&
        src_size.x % dst_size.x == 0u && src_size.y % dst_size.y == 0u)
    {
        const uint ratio_x = src_size.x / dst_size.x;
        const uint ratio_y = src_size.y / dst_size.y;
        if (ratio_x >= 1u && ratio_x <= 4u &&
            ratio_y >= 1u && ratio_y <= 4u)
        {
            float4 exact_sum = 0.0;
            const uint2 base = dst_xy * uint2(ratio_x, ratio_y);
            [unroll]
            for (uint y = 0u; y < 4u; ++y)
            {
                [unroll]
                for (uint x = 0u; x < 4u; ++x)
                {
                    if (x < ratio_x && y < ratio_y)
                        exact_sum += source.Load(int3(base + uint2(x, y), 0));
                }
            }
            return exact_sum / (float)(ratio_x * ratio_y);
        }
    }

    const float2 src_min = float2(dst_xy) *
                           (float2(src_size) / float2(dst_size));
    const float2 src_max = (float2(dst_xy) + 1.0) *
                           (float2(src_size) / float2(dst_size));

    const int x0 = max(0, (int)floor(src_min.x));
    const int y0 = max(0, (int)floor(src_min.y));
    const int x1 = min((int)src_size.x - 1, (int)ceil(src_max.x) - 1);
    const int y1 = min((int)src_size.y - 1, (int)ceil(src_max.y) - 1);

    float4 accum = 0.0;
    float total_weight = 0.0;

    for (int y = y0; y <= y1; ++y) {
        const float wy = max(0.0,
                             min(src_max.y, (float)y + 1.0) -
                             max(src_min.y, (float)y));
        if (wy <= 0.0)
            continue;

        for (int x = x0; x <= x1; ++x) {
            const float wx = max(0.0,
                                 min(src_max.x, (float)x + 1.0) -
                                 max(src_min.x, (float)x));
            if (wx <= 0.0)
                continue;

            const float weight = wx * wy;
            accum += source.Load(int3(x, y, 0)) * weight;
            total_weight += weight;
        }
    }

    return accum / max(total_weight, 1e-8);
}

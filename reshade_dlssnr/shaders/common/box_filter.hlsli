#pragma once
#ifndef DLSSNR_BOX_FILTER_HLSLI
#define DLSSNR_BOX_FILTER_HLSLI

float4 BoxAreaAverage(Texture2D<float4> source,
                      uint2 dst_xy,
                      uint2 dst_size,
                      uint2 src_size)
{
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

#endif

#pragma once

#include "bindings.hlsli"

// Returns the motion vector for `pixel_xy`, in motion-texture pixels, together
// with `found`. `found` is separate from the vector value because a legitimately
// stationary pixel yields (0, 0) and must not be mistaken for "no data".
//
// Reads the centre texel and widens to 3x3 only when the centre has no data.
// Taking the smallest-magnitude neighbour instead biases motion toward zero
// wherever an edge shares the neighbourhood, so the reprojected edit lags the
// content it belongs to.
void DilateMotion(Texture2D<float2> motion_tex,
                  int2 pixel_xy,
                  float2 motion_base, float2 motion_size,
                  uint motion_w, uint motion_h,
                  out float2 out_mv, out bool out_found)
{
    out_mv = float2(0.0, 0.0);
    out_found = false;
    const int2 motion_max = int2(motion_w, motion_h) - 1;
    const float2 inv_size = float2(Width, Height);
    float best_mag = 1.0e30;

    int radius = 0;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        [loop]
        for (int dy = -radius; dy <= radius; ++dy)
        {
            [loop]
            for (int dx = -radius; dx <= radius; ++dx)
            {
                const float2 sample_uv =
                    (float2(pixel_xy + int2(dx, dy)) + 0.5) / inv_size;
                const float2 motion_px = motion_base + sample_uv * motion_size;
                if (any(motion_px < 0.0) ||
                    any(motion_px >= float2(motion_w, motion_h)))
                    continue;
                const int2 mxy = clamp(int2(motion_px), int2(0, 0), motion_max);
                const float2 mv = motion_tex.Load(int3(mxy, 0));
                if (!all(isfinite(mv))) continue;
                const float mag = length(mv);
                if (mag < best_mag)
                {
                    best_mag = mag;
                    out_mv = mv;
                    out_found = true;
                }
            }
        }
        if (out_found) break;
        radius = 1;
    }
}

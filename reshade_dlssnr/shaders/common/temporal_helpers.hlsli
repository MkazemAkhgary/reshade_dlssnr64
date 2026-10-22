#pragma once

// Shared temporal reprojection helpers: YCoCg colour space, variance
// clipping (Playdead/Salvi, Karis 2014), and 5-tap Catmull-Rom history
// sampling. Included by reproject_edit.hlsl and commit_history.hlsl so the
// transform and the clip can never drift apart.

// ---------------------------------------------------------------------------
// YCoCg: a reversible colour transform whose luma (Y) is decoupled from
// chroma (Co, Cg). Variance clipping in this space produces less colour
// shift than the equivalent box in RGB, and is the standard TAA choice.
// ---------------------------------------------------------------------------

float3 RGB_to_YCoCg(float3 c)
{
    return float3(
         0.25 * c.r + 0.50 * c.g + 0.25 * c.b,
         0.50 * c.r                - 0.50 * c.b,
        -0.25 * c.r + 0.50 * c.g - 0.25 * c.b);
}

float3 YCoCg_to_RGB(float3 c)
{
    return float3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z);
}

// ---------------------------------------------------------------------------
// Variance clipping.
//
// Compute the mean and standard deviation of the valid history samples in
// either a square neighbourhood or a plus-shape (center row + center
// column) around `center_xy`, then clamp `history` to mean +/- stddev in
// YCoCg. With radius 1 and plus_shape = true the shape is the 5-tap cross
// (center + N/S/E/W). With radius 2 it is the 9-tap 5x5 plus. `confidence` reports how far the clamp moved the
// value: 1 = the history was already inside the box, 0 = it was clipped by a
// full box extent or more. That confidence is what the caller uses to
// speed up history replacement when the reprojected edit is not supported by
// its local neighbourhood.
// ---------------------------------------------------------------------------
float4 VarianceClipNeighbourhood(
    Texture2D<float4> history_tex,
    int2 center_xy,
    int radius,
    bool plus_shape,
    float variance_gamma,
    float4 history,
    out float confidence)
{
    uint2 tex_size;
    history_tex.GetDimensions(tex_size.x, tex_size.y);
    confidence = 0.0;
    if (tex_size.x == 0 || tex_size.y == 0) return history;

    const int2 tex_max = int2(tex_size) - 1;
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    int count = 0;

    [loop]
    for (int dy = -radius; dy <= radius; ++dy)
    {
        [loop]
        for (int dx = -radius; dx <= radius; ++dx)
        {
            // Plus-shape: skip off-axis samples. Diagonal and 2-D
            // neighbours more often straddle an edge than the cardinal
            // ones, so excluding them gives a cleaner variance estimate
            // even at a lower tap count.
            if (plus_shape && dx != 0 && dy != 0) continue;
            const int2 p = clamp(center_xy + int2(dx, dy), int2(0, 0), tex_max);
            const float4 e = history_tex.Load(int3(p, 0));
            if (e.a <= 0.01) continue;  // invalid history sample
            const float3 yc = RGB_to_YCoCg(e.rgb);
            m1 += yc;
            m2 += yc * yc;
            ++count;
        }
    }

    if (count == 0) return history;

    const float inv = 1.0 / (float)count;
    const float3 mean     = m1 * inv;
    const float3 variance = max(m2 * inv - mean * mean, float3(0.0, 0.0, 0.0));
    const float3 stddev   = sqrt(variance);
    // The half-extent is gamma * stddev. Gamma = 1 is the classic Salvi
    // clip; larger values loosen the box, which is necessary for shadowed
    // regions where the local variance collapses and a tight box resets
    // the whole history every few frames (visible as a dim/bright strobe).
    const float3 extent   = stddev * max(variance_gamma, 0.25);

    const float3 history_yc = RGB_to_YCoCg(history.rgb);
    const float3 clamped_yc = clamp(history_yc, mean - extent, mean + extent);

    // Confidence: how close the clamped value is to the original. A large
    // clamp distance means the history disagreed with its own neighbourhood
    // and should be replaced quickly.
    const float3 safe_extent = max(extent, float3(0.01, 0.01, 0.01));
    const float  norm_dist   = length(abs(clamped_yc - history_yc) / safe_extent);
    confidence = saturate(1.0 - norm_dist);

    return float4(YCoCg_to_RGB(clamped_yc), history.a);
}

// ---------------------------------------------------------------------------
// 5-tap Catmull-Rom history sampling.
//
// Bilinear sampling is C0: its derivative jumps at every texel boundary,
// which is visible as a frame-to-frame step ("texel snapping") under
// sub-pixel motion. Catmull-Rom is C1 and removes that step. This is the
// 5-bilinear-tap form from JMS55/Bevy (also used by Godot and EEVEE-Next).
//
// `tex_size` is the history texture's width/height in texels.
// ---------------------------------------------------------------------------
float4 SampleHistoryCatmullRom(
    Texture2D<float4> history_tex,
    SamplerState samp,
    float2 uv,
    float2 tex_size)
{
    const float2 sample_pos = uv * tex_size;
    const float2 texel_center = floor(sample_pos - 0.5) + 0.5;
    const float2 f = sample_pos - texel_center;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;

    const float2 texel_pos0  = (texel_center - 1.0) / tex_size;
    const float2 texel_pos3  = (texel_center + 2.0) / tex_size;
    const float2 texel_pos12 = (texel_center + w2 / w12) / tex_size;

    float4 result = 0.0;
    result += history_tex.SampleLevel(samp, float2(texel_pos12.x, texel_pos0.y), 0.0) * w12.x * w0.y;
    result += history_tex.SampleLevel(samp, float2(texel_pos0.x,  texel_pos12.y), 0.0) * w0.x  * w12.y;
    result += history_tex.SampleLevel(samp, float2(texel_pos12.x, texel_pos12.y), 0.0) * w12.x * w12.y;
    result += history_tex.SampleLevel(samp, float2(texel_pos3.x,  texel_pos12.y), 0.0) * w3.x  * w12.y;
    result += history_tex.SampleLevel(samp, float2(texel_pos12.x, texel_pos3.y), 0.0) * w12.x * w3.y;
    return result;
}

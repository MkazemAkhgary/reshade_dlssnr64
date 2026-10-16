#include "common/bindings.hlsli"

Texture2D<float4> gCandidate : register(t0);
Texture2D<float2> gMotion : register(t1);
Texture2D<float> gCurrentDepth : register(t2);
Texture2D<float> gPreviousDepth : register(t3);
RWTexture2D<float4> gValidated : register(u0);
RWBuffer<uint> gStats : register(u1);

void WriteValidated(uint2 pixel, float4 value)
{
    gValidated[pixel] = value;
    if (value.a != 1.0)
    {
        uint ignored;
        InterlockedAdd(gStats[0], 1u, ignored);
    }
    if (pixel.x == 0u && pixel.y == 0u)
    {
        uint ignored;
        InterlockedOr(gStats[1], 1u, ignored);
    }
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const uint2 pixel = id.xy;
    const float4 candidate = gCandidate.Load(int3(pixel, 0));
    if (candidate.a < 0.5)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
    uint mw, mh, cw, ch;
    gMotion.GetDimensions(mw, mh);
    gCurrentDepth.GetDimensions(cw, ch);
    if (mw == 0 || mh == 0 || cw == 0 || ch == 0)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 3.0));
        return;
    }
    const float2 motion_base = kTemporalMotionBase;
    const float2 motion_size = kTemporalMotionSubrectSize;
    const float2 motion_pixel = motion_base + uv * motion_size;
    if (any(motion_pixel < 0.0) || any(motion_pixel >= float2(mw, mh)))
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    const uint2 mxy = min(uint2(motion_pixel), uint2(mw - 1u, mh - 1u));
    const float2 mv = gMotion.Load(int3(mxy, 0));
    float2 reference_size = kTemporalMotionReferenceSize;
    reference_size = max(reference_size, 1.0);
    const float2 previous_uv = uv + mv * kTemporalMotionScale / reference_size;
    if (!all(isfinite(mv)) || !all(isfinite(previous_uv)) ||
        any(previous_uv < 0.0) || any(previous_uv >= 1.0))
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }
    const float2 guide_base = kTemporalGuideBase;
    const float2 guide_size = kTemporalGuideSize;
    const float2 current_depth_pixel = guide_base + uv * guide_size;
    if (any(current_depth_pixel < 0.0) ||
        any(current_depth_pixel >= float2(cw, ch)))
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 3.0));
        return;
    }
    const uint2 cxy = min(uint2(current_depth_pixel), uint2(cw - 1u, ch - 1u));
    const uint2 pxy = min(uint2(previous_uv * float2(Width, Height)),
                          uint2(Width - 1u, Height - 1u));
    const float d_now = gCurrentDepth.Load(int3(cxy, 0));
    const float d_old = gPreviousDepth.Load(int3(pxy, 0));
    float abs_threshold = kTemporalDepthAbsThreshold;
    abs_threshold = max(abs_threshold, 1.0e-6);
    float rel_threshold = kTemporalDepthRelThreshold;
    rel_threshold = max(rel_threshold, 0.0);
    const float reference_depth = max(abs(d_now), abs(d_old));
    const float relative_allowance = rel_threshold * reference_depth;
    const float threshold = max(abs_threshold, relative_allowance);
    if (!isfinite(d_now) || !isfinite(d_old) || abs(d_now - d_old) > threshold)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 3.0));
        return;
    }
    WriteValidated(pixel, float4(clamp(candidate.rgb, -0.69314718056, 0.69314718056),
                                candidate.a >= 1.5 ? 2.0 : 1.0));
}

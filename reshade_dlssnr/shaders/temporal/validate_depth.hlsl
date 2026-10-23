#include "common/bindings.hlsli"
#include "common/math.hlsli"
#include "common/temporal_helpers.hlsli"
#include "common/temporal_motion.hlsli"

Texture2D<float4> gCandidate     : register(t0);
Texture2D<float2> gMotion        : register(t1);
Texture2D<float>  gCurrentDepth  : register(t2);
Texture2D<float>  gPreviousDepth : register(t3);
RWTexture2D<float4> gValidated   : register(u0);
RWBuffer<uint>      gStats       : register(u1);

// Policy threshold: a pixel is counted "invalid for reuse" below this
// confidence.
static const float kReuseConfidenceThreshold = 0.25;

void WriteValidated(uint2 pixel, float4 value)
{
    gValidated[pixel] = value;
    if (value.a < kReuseConfidenceThreshold)
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
    if (candidate.a <= 0.0)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);

    uint mw = 0, mh = 0, cw = 0, ch = 0;
    gMotion.GetDimensions(mw, mh);
    gCurrentDepth.GetDimensions(cw, ch);
    if (mw == 0 || mh == 0 || cw == 0 || ch == 0)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const float2 motion_base = kTemporalMotionBase;
    const float2 motion_size = kTemporalMotionSubrectSize;

    float2 mv = float2(0.0, 0.0);
    bool   mv_found = false;
    DilateMotion(gMotion, int2(pixel), motion_base, motion_size, mw, mh, mv, mv_found);
    if (!mv_found)
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const float2 ref_size = max(kTemporalMotionReferenceSize, float2(1.0, 1.0));
    const float2 prev_uv  = uv + mv * kTemporalMotionScale / ref_size;
    if (!all(isfinite(prev_uv)) ||
        any(prev_uv < 0.0) || any(prev_uv >= 1.0))
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const float2 guide_base = kTemporalGuideBase;
    const float2 guide_size = kTemporalGuideSize;
    const float2 cur_px = guide_base + uv * guide_size;
    if (any(cur_px < 0.0) || any(cur_px >= float2(cw, ch)))
    {
        WriteValidated(pixel, float4(0.0, 0.0, 0.0, 0.0));
        return;
    }

    const uint2 cxy = min(uint2(cur_px), uint2(cw - 1u, ch - 1u));
    const uint2 pxy = min(
        uint2(prev_uv * float2(Width, Height)),
        uint2(Width - 1u, Height - 1u));
    const float d_now = gCurrentDepth.Load(int3(cxy, 0));
    const float d_old = gPreviousDepth.Load(int3(pxy, 0));

    float conf = candidate.a;
    if (!isfinite(d_now) || !isfinite(d_old))
    {
        conf = 0.0;
    }
    else
    {
        const float abs_tol = max(kTemporalDepthAbsThreshold, 1.0e-6);
        const float rel_tol = max(kTemporalDepthRelThreshold, 0.0);
        const float ref_d   = max(abs(d_now), abs(d_old));
        const float tol     = max(abs_tol, rel_tol * ref_d);
        // Depth is a disocclusion gate, not a confidence measure. Deadzone so
        // the ordinary reprojection error on a moving surface costs nothing.
        conf *= DeadzoneConfidence(abs(d_now - d_old), tol);
    }

    WriteValidated(pixel, float4(clamp(candidate.rgb, -kLn2, kLn2), conf));
}

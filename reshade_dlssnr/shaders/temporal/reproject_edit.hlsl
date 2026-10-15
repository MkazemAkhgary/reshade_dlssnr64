#include "common/bindings.hlsli"

Texture2D<float4> gCurrentInput : register(t0);
Texture2D<float4> gPreviousInput : register(t1);
Texture2D<float4> gPreviousEdit : register(t2);
Texture2D<float2> gMotion : register(t3);
SamplerState gLinearClamp : register(s0);
RWTexture2D<float4> gCandidate : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kLn2 = 0.69314718056;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    uint mw, mh;
    gMotion.GetDimensions(mw, mh);
    if (mw == 0 || mh == 0) { gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0); return; }
    const float2 motion_base = kTemporalMotionBase;
    const float2 motion_size = kTemporalMotionSubrectSize;
    const float2 motion_pixel = motion_base + uv * motion_size;
    if (any(motion_pixel < 0.0) || any(motion_pixel >= float2(mw, mh)))
    {
        gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const uint2 mxy = min(uint2(motion_pixel), uint2(mw - 1u, mh - 1u));
    const float2 mv = gMotion.Load(int3(mxy, 0));
    float2 reference_size = kTemporalMotionReferenceSize;
    reference_size = max(reference_size, 1.0);
    // Contract: NGX motion vectors are current-to-previous displacement in
    // reference-image pixels; apply NGX's MV scales and convert to UV exactly
    // once. Never infer the coordinate space from vector magnitude.
    const float2 delta_uv = mv * kTemporalMotionScale / reference_size;
    const float2 previous_uv = uv + delta_uv;
    const float2 displacement = delta_uv * float2(Width, Height);
    float max_displacement = kTemporalMaxDisplacement;
    max_displacement = max(max_displacement, 1.0);
    if (!all(isfinite(mv)) || !all(isfinite(previous_uv)) ||
        length(displacement) > max_displacement || any(previous_uv < 0.0) ||
        any(previous_uv >= 1.0))
    {
        gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const uint2 previous_xy = min(uint2(previous_uv * float2(Width, Height)),
                                  uint2(Width - 1u, Height - 1u));
    const float4 old_edit = gPreviousEdit.Load(int3(previous_xy, 0));
    if (!all(isfinite(old_edit)) || old_edit.a < 0.5)
    {
        gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float4 current = gCurrentInput.Load(int3(id.xy, 0));
    const float4 previous = gPreviousInput.SampleLevel(gLinearClamp, previous_uv, 0.0);
    if (!all(isfinite(current)) || !all(isfinite(previous)))
    {
        gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float yc = dot(max(current.rgb, 0.0), kLuma);
    const float yp = dot(max(previous.rgb, 0.0), kLuma);
    float abs_threshold = kTemporalPhotometricAbsThreshold;
    abs_threshold = max(abs_threshold, 0.0);
    float rel_threshold = kTemporalPhotometricRelThreshold;
    rel_threshold = max(rel_threshold, 0.0);
    const float relative_allowance = rel_threshold * max(abs(yc), abs(yp));
    const float threshold = abs_threshold + relative_allowance;
    const bool photo_mismatch = abs(yc - yp) > threshold;
    // Alpha 1: eligible; 2: photometric mismatch (still eligible, but counted
    // as invalid feedback); 0: geometry/motion/history rejected.
    gCandidate[id.xy] = float4(clamp(old_edit.rgb, -kLn2, kLn2),
                               photo_mismatch ? 2.0 : 1.0);
}

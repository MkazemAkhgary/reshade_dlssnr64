#include "common/bindings.hlsli"
#include "common/math.hlsli"
#include "common/temporal_helpers.hlsli"
#include "common/temporal_motion.hlsli"

Texture2D<float4> gCurrentInput  : register(t0);
Texture2D<float4> gPreviousInput : register(t1);
Texture2D<float4> gPreviousEdit  : register(t2);
Texture2D<float2> gMotion        : register(t3);
SamplerState      gLinearClamp   : register(s0);
RWTexture2D<float4> gCandidate   : register(u0);
RWBuffer<uint>      gStats       : register(u1);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    // Clear the validation stats on the first pixel. This replaces the
    // separate TemporalClearStats dispatch.
    if (id.x == 0u && id.y == 0u) {
      gStats[0] = 0u;
      gStats[1] = 0u;
      gStats[2] = 0u;
      gStats[3] = 0u;
      gStats[4] = 0u;
    }
    gCandidate[id.xy] = float4(0.0, 0.0, 0.0, 0.0);

    uint mw = 0, mh = 0;
    gMotion.GetDimensions(mw, mh);
    if (mw == 0 || mh == 0) return;

    const float2 motion_base = kTemporalMotionBase;
    const float2 motion_size = kTemporalMotionSubrectSize;

    float2 mv = float2(0.0, 0.0);
    bool   mv_found = false;
    DilateMotion(gMotion, int2(id.xy), motion_base, motion_size, mw, mh, mv, mv_found);
    if (!mv_found) return;

    const float2 uv       = (float2(id.xy) + 0.5) / float2(Width, Height);
    const float2 ref_size = max(kTemporalMotionReferenceSize, float2(1.0, 1.0));
    const float2 delta_uv = mv * kTemporalMotionScale / ref_size;
    const float2 prev_uv  = uv + delta_uv;

    const float max_disp = max(kTemporalMaxDisplacement, 1.0);
    if (length(delta_uv * float2(Width, Height)) > max_disp) return;
    if (any(prev_uv < 0.0) || any(prev_uv >= 1.0)) return;

    // Catmull-Rom sampling: C1-continuous, removes the bilinear texel-boundary
    // derivative discontinuity that shows up as frame-to-frame gain steps.
    float2 history_size;
    gPreviousEdit.GetDimensions(history_size.x, history_size.y);
    const float4 raw_edit = SampleHistoryCatmullRom(
        gPreviousEdit, gLinearClamp, prev_uv, history_size);
    // Alpha is a presence flag, not a weight: a low-confidence history is still
    // usable, only an absent one is not.
    if (!all(isfinite(raw_edit)) || raw_edit.a <= 0.0001) return;

    // Neighbourhood agreement, used ONLY as a confidence signal. The value is
    // never clipped: this buffer is written back as the history on reuse
    // frames, so a clip feeds back and decays the edit toward the local mean.
    const int2 history_center = int2(prev_uv * history_size);
    float clamp_conf = 1.0;
    VarianceClipNeighbourhood(
        gPreviousEdit, history_center, 1, true, 2.0, raw_edit, clamp_conf);
    const float3 edit_rgb = clamp(raw_edit.rgb, -kLn2, kLn2);

    const float4 current  = gCurrentInput.Load(int3(id.xy, 0));
    const float4 previous = gPreviousInput.SampleLevel(gLinearClamp, prev_uv, 0.0);
    if (!all(isfinite(current)) || !all(isfinite(previous))) return;

    const float yc = dot(max(current.rgb,  0.0), kLuma);
    const float yp = dot(max(previous.rgb, 0.0), kLuma);
    const float abs_thr = max(kTemporalPhotometricAbsThreshold, 0.0);
    const float rel_thr = max(kTemporalPhotometricRelThreshold, 0.0);
    const float luma_thr = abs_thr + rel_thr * max(yc, yp);
    const float photo_conf =
        DeadzoneConfidence(abs(yc - yp), luma_thr);

    // Confidence is this frame's agreement only; multiplying in the previous
    // confidence compounds it every frame.
    const float out_conf = clamp_conf * photo_conf;
    gCandidate[id.xy] = float4(edit_rgb, out_conf);
}

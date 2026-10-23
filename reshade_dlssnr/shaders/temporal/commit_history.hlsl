#include "common/bindings.hlsli"
#include "common/math.hlsli"
#include "common/temporal_helpers.hlsli"

Texture2D<float4>  gInput        : register(t0);
Texture2D<float4>  gOutput       : register(t1);
Texture2D<float4>  gValidated    : register(t2);
Texture2D<float>   gCurrentDepth : register(t3);
RWTexture2D<float4> gHistoryEdit : register(u0);
RWTexture2D<float>  gHistoryDepth: register(u1);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float  kSignalFloor = 0.005;

// Fresh edit extracted from this frame's (input, NGX output) pair. Returns
// alpha 0 when there is no valid edit at this pixel (signal below the noise
// floor or non-finite data), in which case the caller must keep the
// reprojected history instead of blending a zero in.
float4 ExtractFreshEdit(uint2 pixel)
{
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    const float4 input  = gInput.Load(int3(pixel, 0));
    const float4 output = gOutput.Load(int3(pixel, 0));
    const float3 a = max(input.rgb, 0.0);
    const float3 b = max(output.rgb, 0.0);
    const float yi = dot(a, kLuma);
    const float yo = dot(b, kLuma);
    if (all(isfinite(input)) && all(isfinite(output)) &&
        yi >= kSignalFloor && yo >= kSignalFloor)
    {
        const float gy      = log(clamp(yo / yi, 0.5, 2.0));
        const float eps     = max(1.0e-4, yi * 0.01);
        const float3 raw    = log((b + eps) / (a + eps));
        const float3 q      = clamp(raw - gy, -kLn2, kLn2);
        const float3 tau    = max(float3(kSignalFloor, kSignalFloor, kSignalFloor), yi * 0.02);
        const float3 omega  = max(float3(0.01, 0.01, 0.01), yi * 0.10);
        const float3 conf   = saturate((min(a, b) - tau) / omega);
        const float3 gain   = clamp(gy + conf * q, -kLn2, kLn2);
        result = float4(gain, 1.0);
    }
    return result;
}

float ReadCurrentDepth(uint2 pixel)
{
    float result = 0.0;
    uint dw = 0, dh = 0;
    gCurrentDepth.GetDimensions(dw, dh);
    if (dw > 0u && dh > 0u)
    {
        const float2 uv   = (float2(pixel) + 0.5) / float2(Width, Height);
        const float2 base = kTemporalGuideBase;
        const float2 size = kTemporalGuideSize;
        const float2 src  = base + uv * size;
        if (all(src >= 0.0) && all(src < float2(dw, dh)))
        {
            const uint2 p = min(uint2(src), uint2(dw - 1u, dh - 1u));
            const float d = gCurrentDepth.Load(int3(p, 0));
            result = isfinite(d) ? d : 0.0;
        }
    }
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const uint2 pixel = id.xy;

    // kTemporalUseValidatedHistory == 1 means the caller skipped NGX and the
    // validated candidate IS the history. In that case there is no fresh
    // edit to blend in: store the candidate with its confidence.
    //
    // kTemporalUseValidatedHistory == 0 means NGX ran this frame, so the
    // fresh edit replaces the stored one.
    if (kTemporalUseValidatedHistory > 0.5)
    {
        const float4 candidate = gValidated.Load(int3(pixel, 0));
        const float  conf      = saturate(candidate.a);
        if (conf > 0.01)
        {
            gHistoryEdit[pixel] = float4(
                clamp(candidate.rgb, -kLn2, kLn2), conf);
        }
        else
        {
            gHistoryEdit[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        }
    }
    else
    {
        // Fresh mode: NGX produced a new output. Accumulate it into history.
        const float4 candidate = gValidated.Load(int3(pixel, 0));
        const float4 fresh     = ExtractFreshEdit(pixel);
        const float  cand_conf = saturate(candidate.a);
        const float  fresh_ok  = fresh.a > 0.01 ? 1.0 : 0.0;

        if (fresh_ok > 0.5)
        {
            // History is the last fresh edit. Averaging it against the
            // reprojected candidate only adds lag, and that lag is what makes
            // a reuse frame differ from the fresh frame that follows.
            // Alpha is a presence flag downstream, not a weight.
            gHistoryEdit[pixel] = float4(
                clamp(fresh.rgb, -kLn2, kLn2), 1.0);
        }
        else if (cand_conf > 0.01)
        {
            // No valid fresh edit (NGX output below signal floor). Keep the
            // reprojected history rather than overwriting it with zeros.
            gHistoryEdit[pixel] = float4(
                clamp(candidate.rgb, -kLn2, kLn2), 1.0);
        }
        else
        {
            gHistoryEdit[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        }
    }

    gHistoryDepth[pixel] = ReadCurrentDepth(pixel);
}

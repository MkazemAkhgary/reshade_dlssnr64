#include "common/bindings.hlsli"
#include "common/math.hlsli"

Texture2D<float4> gCurrentInput : register(t0);
Texture2D<float4> gCandidate    : register(t1);
Texture2D<float4> gUnused2      : register(t2);
Texture2D<float4> gUnused3      : register(t3);
RWTexture2D<float4> gOutput     : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float4 current = gCurrentInput.Load(int3(id.xy, 0));
    const float4 edit    = gCandidate.Load(int3(id.xy, 0));
    const float y        = dot(max(current.rgb, 0.0), kLuma);
    const float conf     = saturate(edit.a);

    if (!all(isfinite(current)) || !all(isfinite(edit)) ||
        conf <= 0.0 || y < 0.005)
    {
        gOutput[id.xy] = current;
        return;
    }

    const float3 gain    = exp(clamp(edit.rgb, -kLn2, kLn2));
    const float3 edited  = max(current.rgb, 0.0) * gain;
    // Confidence gates the edit rather than scaling it linearly. A healthy
    // history sits near 0.95, and using that directly would attenuate the edit
    // by 5% on every reuse frame while the fresh frame applies it fully -- a
    // step at each refresh. Full weight above the knee, fade to zero below.
    const float weight   = saturate((conf - 0.25) / 0.65);
    const float3 blended = lerp(current.rgb, edited, weight);
    gOutput[id.xy] = float4(blended, current.a);
}

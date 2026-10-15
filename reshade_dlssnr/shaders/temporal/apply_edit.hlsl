#include "common/bindings.hlsli"

Texture2D<float4> gCurrentInput : register(t0);
Texture2D<float4> gCandidate : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWTexture2D<float4> gOutput : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kLn2 = 0.69314718056;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float4 current = gCurrentInput.Load(int3(id.xy, 0));
    const float4 edit = gCandidate.Load(int3(id.xy, 0));
    const float y = dot(max(current.rgb, 0.0), kLuma);
    if (!all(isfinite(current)) || !all(isfinite(edit)) ||
        (edit.a != 1.0 && edit.a != 2.0) || y < 0.005)
    {
        gOutput[id.xy] = current;
        return;
    }
    const float3 gain = exp(clamp(edit.rgb, -kLn2, kLn2));
    gOutput[id.xy] = float4(max(current.rgb, 0.0) * gain, current.a);
}

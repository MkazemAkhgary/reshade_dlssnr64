#include "common/bindings.hlsli"
#include "common/math.hlsli"

Texture2D<float4>   gInput     : register(t0);
Texture2D<float4>   gCandidate : register(t1);
Texture2D<float4>   gCheap     : register(t2);
Texture2D<float4>   gUnused3   : register(t3);
RWTexture2D<float4> gOutput    : register(u0);
SamplerState        s0         : register(s0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    const uint2 pixel    = id.xy;
    const float4 current = gInput.Load(int3(pixel, 0));
    const float4 edit    = gCandidate.Load(int3(pixel, 0));
    const float2 uv      = (float2(pixel) + 0.5) / float2(Width, Height);
    const float4 cheap   = gCheap.SampleLevel(s0, uv, 0.0);
    const float  conf    = saturate(edit.a);
    const float  y       = dot(max(current.rgb, 0.0), kLuma);
    const float  alpha   = isfinite(current.a) ? current.a : 1.0;

    const bool current_ok = all(isfinite(current.rgb));
    const bool cheap_ok   = all(isfinite(cheap.rgb));
    const bool edit_ok    = all(isfinite(edit.rgb));

    float3 result = current.rgb;
    if (current_ok && cheap_ok)
    {
        if (edit_ok && conf > 0.0 && y >= 0.005)
        {
            const float3 gain   = exp(clamp(edit.rgb, -kLn2, kLn2));
            const float3 edited = max(current.rgb, 0.0) * gain;
            const float  weight = saturate((conf - 0.3) / 0.45);
            result = lerp(cheap.rgb, edited, weight);
        }
        else
        {
            result = cheap.rgb;
        }
    }
    else if (cheap_ok)
    {
        result = cheap.rgb;
    }

    gOutput[pixel] = float4(result, alpha);
}

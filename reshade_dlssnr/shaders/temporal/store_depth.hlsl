#include "common/bindings.hlsli"

Texture2D<float> gCurrentDepth : register(t0);
Texture2D<float4> gUnused1 : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWTexture2D<float> gHistoryDepth : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    uint dw, dh;
    gCurrentDepth.GetDimensions(dw, dh);
    if (dw == 0 || dh == 0) { gHistoryDepth[id.xy] = 0.0; return; }
    const float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    const float2 base = kTemporalGuideBase;
    const float2 size = kTemporalGuideSize;
    const float2 pixel = base + uv * size;
    if (any(pixel < 0.0) || any(pixel >= float2(dw, dh)))
    {
        gHistoryDepth[id.xy] = 0.0;
        return;
    }
    const uint2 p = min(uint2(pixel), uint2(dw - 1u, dh - 1u));
    const float depth = gCurrentDepth.Load(int3(p, 0));
    gHistoryDepth[id.xy] = isfinite(depth) ? depth : 0.0;
}

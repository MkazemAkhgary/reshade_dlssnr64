#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"

#define kHistogramOffset shader.Params1.w

RWBuffer<uint> gHistogram : register(u0);

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 256u)
        gHistogram[id.x + (uint)(kHistogramOffset + 0.5)] = 0u;
}

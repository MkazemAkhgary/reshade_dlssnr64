#include "common/registers.hlsli"

RWBuffer<uint> gHistogram : register(u0);

[numthreads(768, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 768u)
        gHistogram[id.x] = 0u;
}

#include "common/registers.hlsli"

RWBuffer<uint> gHistogram : register(u0);

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 256u)
        gHistogram[id.x] = 0u;
}

#include "common/registers.hlsli"

RWBuffer<uint> gStats : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x == 0u) {
        gStats[0] = 0xffffffffu; // min sentinel in sortable order
        gStats[1] = 0u;          // max sentinel in sortable order
        gStats[2] = 0u;          // SDR carrier excursion count
        gStats[3] = 0u;          // gain outside configured range
    }
}

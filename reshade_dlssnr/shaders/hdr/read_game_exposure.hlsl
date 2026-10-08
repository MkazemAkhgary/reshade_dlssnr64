#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"

// NGX exposes the game's final exposure as a 1x1 texture. The exact DXGI
// format varies by title, so the SRV is intentionally read as float4 and the
// first component is used.
Texture2D<float4> gExposure : register(t0);
RWBuffer<uint> gOutput : register(u0);

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x != 0 || id.y != 0 || id.z != 0) return;
    const float value = gExposure.Load(int3(0, 0, 0)).x;
    gOutput[0] = asuint(value);
}

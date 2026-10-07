#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"

Texture2D<float4> gPacked : register(t0);
RWBuffer<uint>    gStats  : register(u0);

#define kGainMinLog2 hdr.GainMinLog2
#define kGainMaxLog2 hdr.GainMaxLog2

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float4 packed = gPacked.Load(int3(id.xy, 0));
    float gain = packed.a;
    // NaN/Inf cannot represent a useful gain statistic. Count them as a
    // clipped sample instead of feeding an undefined ordering into the atomics.
    if (gain != gain || abs(gain) > 1e20) {
        InterlockedAdd(gStats[2], 1u);
        return;
    }
    uint gain_bits = asuint(gain);

    // Atomic min/max on positive sortable floats. The gain range includes
    // negatives, so store an order-preserving float transform in uint space.
    uint sortable = (gain_bits & 0x80000000u) != 0u
        ? ~gain_bits
        : (gain_bits ^ 0x80000000u);
    InterlockedMin(gStats[0], sortable);
    InterlockedMax(gStats[1], sortable);

    if (packed.r < 0.0 || packed.r > 1.0 ||
        packed.g < 0.0 || packed.g > 1.0 ||
        packed.b < 0.0 || packed.b > 1.0) {
        InterlockedAdd(gStats[2], 1u);
    }

    if (gain < kGainMinLog2 || gain > kGainMaxLog2) {
        InterlockedAdd(gStats[3], 1u);
    }
}

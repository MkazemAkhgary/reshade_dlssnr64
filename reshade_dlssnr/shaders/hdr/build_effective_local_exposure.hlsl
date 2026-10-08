#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float> gOutput : register(u0);
SamplerState s0 : register(s0);

#define kLocalExposureStrength hdr.LocalExposure.x
#define kLocalMaxBrighten      hdr.LocalExposure.y
#define kLocalMaxDarken        hdr.LocalExposure.z
#define kLocalExposureMidtone hdr.LocalExposure.w

#include "common/hdr_local_exposure.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    // This map is full resolution. It stores the exact combined exposure that
    // GainMapSplit applies to each source pixel: global exposure × local factor.
    // Keeping the original full-resolution field avoids inventing a new local
    // exposure after the model has downsampled its input.
    const float4 src = gSource.Load(int3(id.xy, 0));

    const float3 hdr_linear = DecodeToLinear(
        src.rgb, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    const float3 pre_hdr = HdrPreDesaturate(
        hdr_linear, saturate(kCrosstalk));
    const float raw_luma = max(
        dot(max(pre_hdr, 0.0), HdrLumaWeights()), 0.0);
    const float exposure = max(kExposure, 1e-6);
    const float local_exposure = LocalExposureFactor(
        (float2(id.xy) + 0.5) / float2(max(Width, 1u), max(Height, 1u)),
        raw_luma, exposure);

    gOutput[id.xy] = exposure * local_exposure;
}

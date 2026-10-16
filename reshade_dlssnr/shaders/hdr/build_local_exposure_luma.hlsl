#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float> gOutput : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float3 encoded = gSource.Load(int3(id.xy, 0)).rgb;
    const float3 linear_rgb = DecodeToLinear(
        encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    const float3 pre_hdr = HdrPreDesaturate(linear_rgb, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(pre_hdr, 0.0), HdrLumaWeights()), 0.0);
    const float exposed_luma = hdr_luma * max(kExposure, 1e-6);
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = exposed_luma * kPqPeakNits / effective_peak_nits;
    const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 1e-8);
    gOutput[id.xy] = y_sdr;
}

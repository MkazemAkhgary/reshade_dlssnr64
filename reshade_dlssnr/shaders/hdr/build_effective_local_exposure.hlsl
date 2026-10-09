#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gSource : register(t0);
Texture2D<float> gMask : register(t1);
Texture2D<float> gDepth : register(t2);
RWTexture2D<float> gOutput : register(u0);
SamplerState s0 : register(s0);

#include "common/hdr_local_exposure.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    if (kLocalExposureStrength <= 0.0) {
        gOutput[id.xy] = 1.0;
        return;
    }

    const float3 encoded = gSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_linear = DecodeToLinear(
        encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    const float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));
    const float3 luma_weights = HdrLumaWeights();
    const float source_hdr_luma = max(dot(max(hdr_linear, 0.0), luma_weights), 0.0);
    const float hdr_luma = max(dot(max(pre_hdr, 0.0), luma_weights), 0.0);
    const float exposure = max(kExposure, 1e-6);
    const float y_exposed = hdr_luma * exposure;
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    const float2 uv = (float2(id.xy) + 0.5) /
                      float2(max(Width, 1u), max(Height, 1u));
    gOutput[id.xy] = LocalExposureScale(uv, y_sdr, source_hdr_luma);
}

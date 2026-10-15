#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/box_filter.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

#include "common/hdr_local_exposure.hlsli"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    const float4 src = BoxAreaAverage(gSource, id.xy, uint2(Width, Height),
                                      uint2(src_width, src_height));
    const float3 luma_weights = HdrLumaWeights();
    const float3 hdr_linear = DecodeToLinear(
        src.rgb, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    const float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(pre_hdr, 0.0), luma_weights), 0.0);

    const float exposure = max(kExposure, 1e-6);
    const float y_exposed = hdr_luma * exposure;
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    const float chroma_scale = y_exposed > 0.0 ? y_sdr / y_exposed : 0.0;
    // The pre-desaturation transform is linear and immediately inverted after
    // tone mapping. Their product is the identity, so apply the derived scalar
    // directly to the source RGB. This avoids cancellation noise in near-zero
    // channels of bright saturated colors without changing the intended mapping.
    const float3 global_sdr = max(hdr_linear, 0.0) *
                              (exposure * chroma_scale);
    const float2 uv = (float2(id.xy) + 0.5) /
                      float2(max(Width, 1u), max(Height, 1u));
    const float local_scale = LocalExposureScale(uv, y_sdr);
    const float global_scale = exposure * chroma_scale;
    const float carrier_scale = global_scale * local_scale;
    const float3 sdr = max(hdr_linear, 0.0) * carrier_scale;

    // RGB was formed by multiplying the source by carrier_scale. Store its
    // direct reciprocal, avoiding a second luminance reduction and preserving
    // a single scalar relationship for all channels, including super-whites.
    // RGBA32F keeps the multiplier linear; no log/exp conversion or ceiling.
    const float carrier_to_hdr_scale = carrier_scale > 0.0
        ? rcp(carrier_scale)
        : 1.0;
    gOutput[id.xy] = float4(sdr, carrier_to_hdr_scale);
}

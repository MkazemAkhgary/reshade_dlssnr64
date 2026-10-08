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

    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    float4 src = BoxAreaAverage(gSource, id.xy, uint2(Width, Height),
                                uint2(src_width, src_height));

    const float3 luma = HdrLumaWeights();
    float3 hdr_linear = DecodeToLinear(
        src.rgb, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));

    // First apply the single global eye-adaptation exposure, then map HDR to
    // the linear SDR carrier with BT.2446 Method C. Local exposure is a
    // separate base/detail operation in this SDR domain.
    const float exposure = max(kExposure, 1e-6);
    const float hdr_luma = max(dot(max(pre_hdr, 0.0), luma), 0.0);
    const float y_exposed = hdr_luma * exposure;
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    const float chroma_scale =
        y_exposed > 1e-8 ? y_sdr / y_exposed : 0.0;
    const float3 pre_sdr = pre_hdr * exposure * chroma_scale;
    const float3 global_sdr =
        HdrInversePreDesaturate(pre_sdr, saturate(kCrosstalk));

    const float local_scale = LocalExposureScale(
        (float2(id.xy) + 0.5) / float2(max(Width, 1u), max(Height, 1u)),
        y_sdr);
    const float3 sdr = max(global_sdr * local_scale, 0.0);

    // The gain is derived from the actual HDR and final SDR carrier lumas so
    // the (carrier, gain) pair remains a lossless reconstruction of the source
    // even after local exposure has been applied.
    const float y_hdr_gain = max(dot(max(hdr_linear, 0.0), luma), 0.0);
    const float y_sdr_gain = max(dot(sdr, luma), 0.0);
    const float gain = log2(
        max(y_hdr_gain + kAlternateOffset, 1e-12) /
        max(y_sdr_gain + kBaseOffset, 1e-12));

    gOutput[id.xy] = float4(sdr, gain);
}

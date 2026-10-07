#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/box_filter.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);


float3 PreDesaturate(float3 c, float a)
{
    // Reference crosstalk matrix:
    //   [1-2a, a,    a   ]
    //   [a,    1-2a, a   ]
    //   [a,    a,    1-2a]
    // The cross-channel term is therefore a * (sum - channel). Adding
    // a * sum directly would silently change the diagonal to 1-a, making
    // InversePreDesaturate no longer the inverse and producing hue shifts.
    float d = 1.0 - 2.0 * a;
    float sum = c.r + c.g + c.b;
    return float3(d * c.r + a * (sum - c.r),
                  d * c.g + a * (sum - c.g),
                  d * c.b + a * (sum - c.b));
}

float3 InversePreDesaturate(float3 c, float a)
{
    float denom = max(1.0 - 3.0 * a, 1e-6);
    float off = -a / denom;
    float diag = 1.0 - 2.0 * off;
    float sum = c.r + c.g + c.b;
    return float3(diag * c.r + off * (sum - c.r),
                  diag * c.g + off * (sum - c.g),
                  diag * c.b + off * (sum - c.b));
}

float Bt2446C(float y_nits)
{
    float y = max(y_nits, 0.0);
    float y_ip = kBt2446K4 / max(kBt2446K1, 1e-6);
    float result = kBt2446K1 * y;
    if (y >= y_ip)
        result = kBt2446K2 * log(max(y / y_ip, 1e-6)) + kBt2446K4;
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    // When the split itself is evaluated below the source resolution (the skin
    // detector path), use the same exact footprint box as the main SDR model
    // input path. At 1:1 resolution this collapses to a single source texel.
    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    float4 src = BoxAreaAverage(gSource, id.xy, uint2(Width, Height),
                                uint2(src_width, src_height));

    const float3 luma = HdrLumaWeights();
    float3 hdr_linear = DecodeToLinear(
        src.rgb, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre_hdr = PreDesaturate(hdr_linear, saturate(kCrosstalk));

    // Exposure is a scene-adaptation control. It changes the image presented
    // to the SDR model, but never becomes part of the alternate HDR image or
    // its gain-map reconstruction.
    float exposure = max(kExposure, 1e-6);
    float3 pre_exposed = pre_hdr * exposure;
    float y_exposed = max(dot(pre_exposed, luma), 0.0);

    // The gain-map working domain is normalized to SDR white = 100 nits.
    // The CPU has already converted the measured 80-nit scRGB-domain scene
    // white into absolute nits, so there is one explicit domain crossing.
    float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    float y_sdr_pct = Bt2446C(y_curve);
    float y_sdr = max(y_sdr_pct / 100.0, 0.0);

    // BT.2446C is used as a luma-only curve. Preserve chromaticity by scaling
    // the entire RGB triple by the luma ratio, then undo the reference
    // crosstalk matrix. This is the defining LumaGainMapSplitter architecture:
    // a single scalar gain map must not be used to solve RGB channels
    // independently with a constant per-channel offset, because those offsets
    // pull dark chromatic pixels toward neutral grey and make highlights pale.
    float chroma_scale = y_exposed > 1e-8 ? y_sdr / y_exposed : 0.0;
    float3 pre_sdr = pre_exposed * chroma_scale;
    float3 sdr =
        InversePreDesaturate(pre_sdr, saturate(kCrosstalk));

    // The packed RGB is the lossless linear SDR carrier. Do NOT gamut-fit or
    // clamp it here. At elevated exposure, BT.2446C can legitimately produce
    // super-white SDR values, and those values are part of the reversible
    // HDR->(SDR, gain) split. The model-facing path applies a hue-preserving
    // gamut fit later, after this carrier and its gain have been stored.

    // The gain is derived from the actual linear HDR and lossless SDR carrier
    // lumas, not from the temporary exposure-adjusted HDR sample.
    float y_hdr_gain = max(dot(max(hdr_linear, 0.0), luma), 0.0);
    float y_sdr_gain = max(dot(sdr, luma), 0.0);
    float gain = log2(
        max(y_hdr_gain + kAlternateOffset, 1e-12) /
        max(y_sdr_gain + kBaseOffset, 1e-12));

    gOutput[id.xy] = float4(sdr, gain);
}

#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"
#include "common/compose_common.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4>   gOriginal    : register(t0);
Texture2D<float4>   gLowInput    : register(t1);
Texture2D<float4>   gLowEnhanced : register(t2);
Texture2D<float4>   gHdrSource   : register(t3);
RWTexture2D<float4> gOutput      : register(u0);
SamplerState        s0           : register(s0);

#include "common/compose_upsample.hlsli"

#define kLocalExposureStrength hdr.LocalExposure.x

float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}

float3 EncodeHdr(float3 lin)
{
    return HdrEncodeTarget(lin, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
}

// Gain-Map Residual measures the model edit as a per-channel log ratio of
// its SDR input and output and applies it multiplicatively to the composition
// base. Do not invert the nonlinear gamut fit on both endpoints: near white,
// tiny model changes would become enormous HDR deltas.
float3 ComposeHdrInverse(float3 sdr_base, float3 hdr_src,
                         float3 low_in, float3 low_out)
{
    float3 composition_base = hdr_src;

    // In this mode the residual is measured against the actual Grade Model
    // Input result, then applied to the image just before that grade. The
    // carrier already includes local exposure, so invert only the global
    // carrier scale here; using the packed reciprocal would cancel the local
    // exposure that belongs to the pre-grade image.
    if (kApplyEditToPreGrade > 0.5 && kLocalExposureStrength > 0.0)
    {
        const float3 pre_hdr = HdrPreDesaturate(
            max(hdr_src, 0.0), saturate(kCrosstalk));
        const float hdr_luma = max(dot(pre_hdr, HdrLumaWeights()), 0.0);
        const float exposure = max(kExposure, 1e-6);
        const float y_exposed = hdr_luma * exposure;
        const float effective_peak_nits = max(
            kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
        const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
        const float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
        const float global_scale = y_exposed > 0.0
            ? exposure * (y_sdr / y_exposed)
            : 0.0;
        const float inverse_global_scale = rcp(max(global_scale, 1e-8));
        composition_base = max(sdr_base, 0.0) * inverse_global_scale;
    }

    float3 result = composition_base;
    const float3 low_in_lin = max(SrgbToLinearExt(low_in), 0.0);
    const float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);

    float3 log_gain = safe_log3(low_out_lin) - safe_log3(low_in_lin);
    if (kRatioClampsEnabled) {
        log_gain = clamp(log_gain,
                         log(max(kRatioDarken, 1e-4)),
                         log(max(kRatioBrighten, 1e-4)));
    }
    if (!all(log_gain == 0.0))
    {
        const float3 kL = HdrLumaWeights();
        const float3 model_sdr_base = GamutFitModelInput(max(sdr_base, 0.0));
        float base_luma = dot(model_sdr_base, kL);
        if (kLocalExposureStrength > 0.0)
            base_luma = max(dot(low_in_lin, kL), 0.0);

        const float3 shaped = ShapeResidual(log_gain, base_luma);
        const float3 hdr_edited = max(
            composition_base * exp(shaped), 0.0);
        result = BlendYCbCr(
            composition_base, hdr_edited, kLumaStrength, kChromaStrength);
    }
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);

    float4 packed = gOriginal.Load(int3(id.xy, 0));
    const float3 source_encoded = gHdrSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_src = DecodeToLinear(
        source_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);

    // Reconstruct both model-domain images with the fixed joint-bilateral 3x3 filter.
    float3 low_in = float3(0.0, 0.0, 0.0);
    float3 low_out = float3(0.0, 0.0, 0.0);
    uint low_w = 0u;
    uint low_h = 0u;
    gLowInput.GetDimensions(low_w, low_h);
    if (low_w == Width && low_h == Height) {
        low_in = gLowInput.Load(int3(id.xy, 0)).rgb;
        low_out = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
    } else {
        const int2 full_size_int = int2((int)Width, (int)Height);
        const int2 full_xy = int2((int)id.x, (int)id.y);
        JointBilateralUpsamplePair(
            full_xy, full_size_int, gLowInput, gLowEnhanced, gOriginal,
            kUpsampleSharpness, kUpsampleSharpness, low_in, low_out);
    }

    const float3 sdr_base = DecodePackedHdr(packed);
    const float3 final_lin = ComposeHdrInverse(
        sdr_base, hdr_src, low_in, low_out);
    const float3 encoded_result = EncodeHdr(final_lin);
    // Preserve native-format codes in untouched channels rather than running
    // their source values through a lossy decode/encode pair.
    const float3 final_out = float3(
        final_lin.r == hdr_src.r ? source_encoded.r : encoded_result.r,
        final_lin.g == hdr_src.g ? source_encoded.g : encoded_result.g,
        final_lin.b == hdr_src.b ? source_encoded.b : encoded_result.b);
    gOutput[id.xy] = float4(final_out, 1.0);
}

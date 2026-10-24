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
//
// `base_scale` carries the local-exposure lift. It is built at working
// resolution by the caller so it holds no full-resolution contrast, which is
// what keeps the lift from acting as a sharpening or softening filter.
float3 ComposeHdrInverse(float3 sdr_base, float3 hdr_src, float3 base_scale,
                         float3 low_in, float3 low_out)
{
    float3 composition_base = max(hdr_src, 0.0) * base_scale;

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

    const float3 source_encoded = gHdrSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_src = DecodeToLinear(
        source_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);

    // Under apply-to-pre-grade with local exposure the host binds the
    // working-resolution local gain to t0 instead of the packed carrier, and
    // the raw HDR source becomes the guide: the gain has to stay at working
    // resolution, so it is reconstructed with the same joint-bilateral filter
    // the model endpoints use. The packed carrier is unused in that branch
    // because the region-protection luma comes from the model input.
    const bool pre_grade_lift =
        kApplyEditToPreGrade > 0.5 && kLocalExposureStrength > 0.0;
    const int2 full_size_int = int2((int)Width, (int)Height);
    const int2 full_xy = int2((int)id.x, (int)id.y);

    float3 low_in = float3(0.0, 0.0, 0.0);
    float3 low_out = float3(0.0, 0.0, 0.0);
    float3 base_scale = float3(1.0, 1.0, 1.0);
    uint low_w = 0u;
    uint low_h = 0u;
    gLowInput.GetDimensions(low_w, low_h);
    const bool identity_scale = (low_w == Width && low_h == Height);
    if (pre_grade_lift) {
        if (identity_scale) {
            low_in = gLowInput.Load(int3(id.xy, 0)).rgb;
            low_out = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
            base_scale = gOriginal.Load(int3(id.xy, 0)).rgb;
        } else {
            JointBilateralUpsamplePair(
                full_xy, full_size_int, gLowInput, gLowEnhanced, gHdrSource,
                kUpsampleSharpness, kUpsampleSharpness, low_in, low_out);
            base_scale = exp(JointBilateralUpsampleLog(
                full_xy, full_size_int, gOriginal, gHdrSource,
                kUpsampleSharpness));
        }
    } else if (identity_scale) {
        low_in = gLowInput.Load(int3(id.xy, 0)).rgb;
        low_out = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
    } else {
        JointBilateralUpsamplePair(
            full_xy, full_size_int, gLowInput, gLowEnhanced, gOriginal,
            kUpsampleSharpness, kUpsampleSharpness, low_in, low_out);
    }

    float3 sdr_base = float3(0.0, 0.0, 0.0);
    if (!pre_grade_lift) {
        sdr_base = DecodePackedHdr(gOriginal.Load(int3(id.xy, 0)));
    }
    const float3 final_lin = ComposeHdrInverse(
        sdr_base, hdr_src, base_scale, low_in, low_out);
    const float3 encoded_result = EncodeHdr(final_lin);
    // Preserve native-format codes in untouched channels rather than running
    // their source values through a lossy decode/encode pair.
    const float3 final_out = float3(
        final_lin.r == hdr_src.r ? source_encoded.r : encoded_result.r,
        final_lin.g == hdr_src.g ? source_encoded.g : encoded_result.g,
        final_lin.b == hdr_src.b ? source_encoded.b : encoded_result.b);
    gOutput[id.xy] = float4(final_out, 1.0);
}

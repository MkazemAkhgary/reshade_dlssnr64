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

#define kLocalExposureStrength hdr.LocalExposure.x

#include "common/fsr.hlsli"

float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}


float3 EncodeHdr(float3 lin)
{
    return HdrEncodeTarget(lin, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
}

// Inverse-tone-map shader reconstructs the model output as a scene-referred HDR
// target. It recovers the exact local multiplier from the original HDR source and
// the carrier generated from that source, without consulting a second estimator.
float3 ComposeHdrInverse(float3 sdr_base, float3 hdr_src,
                         float3 low_in, float3 low_out, float local_sdr_scale)
{
    float3 low_in_lin = max(SrgbToLinearExt(low_in), 0.0);
    float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);
    if (kGradeEnabled > 0.5)
    {
        // GradeModelInput changes the actual model input before NGX runs.
        // Inverse Tone Map reconstructs the model residual from both endpoints,
        // so both endpoints must first be mapped back to the same ungraded
        // model domain. Undoing the grade on only the output makes an identity
        // model look like a color edit whenever the input grade is non-neutral.
        low_in_lin = max(GradeLinearInv(low_in_lin), 0.0);
        low_out_lin = max(GradeLinearInv(low_out_lin), 0.0);
    }
    if (all(low_in_lin == low_out_lin))
        return hdr_src;

    const float3 model_sdr_base = GamutFitModelInput(sdr_base);
    const float3 kL = HdrLumaWeights();
    const float base_luma = dot(max(model_sdr_base, 0.0), kL);
    // Reconstruct both model-domain endpoints in scene-referred HDR using the
    // exact exposure that created the model input. The edit is the model
    // output minus model input, not model output minus the original HDR. That
    // distinction preserves high-frequency source detail when Working Scale
    // is below 1.0 while still transferring the model's scene-referred edit.
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float local_scale = max(local_sdr_scale, 1e-6);
    const float3 low_in_linear =
        GamutUnfitModelInput(max(low_in_lin, 0.0)) / local_scale;
    const float3 low_out_linear =
        GamutUnfitModelInput(max(low_out_lin, 0.0)) / local_scale;
    const float3 hdr_model_in = HdrFromLinearSdrCarrier(
        low_in_linear, kExposure, effective_peak_nits);
    const float3 hdr_model_out = HdrFromLinearSdrCarrier(
        low_out_linear, kExposure, effective_peak_nits);

    // Express the scene-referred model edit as a relative gain. Applying an
    // absolute endpoint difference (hdr_model_out - hdr_model_in) to the source
    // makes highlight growth depend on the inverse transform's reconstructed
    // endpoint range; once that estimate stops growing, composed highlights
    // appear capped. The relative gain transfers the model's edit onto the
    // untouched source, preserving its full HDR headroom and fine detail.
    // FP32 NGX working textures reduce quantization noise before this inverse.
    const float3 delta_scale = max(abs(hdr_model_in), 0.001.xxx);
    const float3 normalized_delta = (hdr_model_out - hdr_model_in) / delta_scale;
    const float3 shaped = ShapeResidual(normalized_delta, base_luma);
    const float3 hdr_edited = max(hdr_src * max(1.0.xxx + shaped, 0.0.xxx), 0.0);

    return BlendYCbCr(hdr_src, hdr_edited, kLumaStrength, kChromaStrength);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);

    float4 packed = gOriginal.SampleLevel(s0, uv, 0);
    const float3 source_encoded = gHdrSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_src = DecodeToLinear(
        source_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);

    // EASU upsample from the actual model working resolution back to full HDR
    // resolution. Both textures are in the same sRGB-encoded model domain.
    const float2 easu_con3 = float2(0.0, 4.0 * kEasuCon1.y);
    float3 low_in = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                             easu_con3, 0);
    float3 low_out = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                              easu_con3, 1);

    if (kRcasEnabled > 0.5) {
        const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);

        float3 b_in = gLowInput.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_in = gLowInput.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_in = gLowInput.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_in = gLowInput.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_in = FsrRcasF(b_in, d_in, low_in, f_in, h_in, kRcasStrength);

        float3 b_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_out = gLowEnhanced.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_out = gLowEnhanced.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_out = FsrRcasF(b_out, d_out, low_out, f_out, h_out, kRcasStrength);
    }

    const float3 sdr_base = DecodePackedHdr(packed);
    float local_sdr_scale = 1.0;
    if (kLocalExposureStrength > 0.0) {
        const float effective_peak_nits = max(
            kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
        const float3 global_sdr = HdrToGlobalLinearSdrCarrier(
            hdr_src, kExposure, effective_peak_nits);
        const float global_luma = max(dot(global_sdr, HdrLumaWeights()), 0.0);
        const float carrier_luma = max(dot(sdr_base, HdrLumaWeights()), 0.0);
        if (global_luma > 0.0)
            local_sdr_scale = carrier_luma / global_luma;
    }
    const float3 final_lin = ComposeHdrInverse(
        sdr_base, hdr_src, low_in, low_out, max(local_sdr_scale, 1e-20));
    const float3 encoded_result = EncodeHdr(final_lin);
    // Preserve native-format codes in untouched channels rather than running
    // their source values through a lossy decode/encode pair.
    const float3 final_out = float3(
        final_lin.r == hdr_src.r ? source_encoded.r : encoded_result.r,
        final_lin.g == hdr_src.g ? source_encoded.g : encoded_result.g,
        final_lin.b == hdr_src.b ? source_encoded.b : encoded_result.b);
    gOutput[id.xy] = float4(final_out, 1.0);
}

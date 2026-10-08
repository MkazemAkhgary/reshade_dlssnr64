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
Texture2D<float>    gEffectiveExposure : register(t3);
RWTexture2D<float4> gOutput      : register(u0);
SamplerState        s0           : register(s0);

#define kLocalExposureStrength hdr.LocalExposure.x
#define kEffectiveExposureMapValid shader.Params1.w

#include "common/fsr.hlsli"

float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}


float3 ReconstructHdr(float3 sdr_base, float gain)
{
    // A is a single luma gain. Apply the ISO offsets to the luma, recover the
    // original HDR luma, then rescale the SDR RGB triple so chromaticity is
    // preserved. Applying the scalar gain independently to RGB with the
    // offsets would add the same offset to every channel and desaturate dark
    // colours.
    const float3 luma = HdrLumaWeights();
    float y_sdr = max(dot(max(sdr_base, 0.0), luma), 0.0);
    float y_hdr = (y_sdr + kBaseOffset) * exp2(gain) - kAlternateOffset;
    y_hdr = max(y_hdr, 0.0);
    float scale = y_sdr > 1e-8 ? y_hdr / y_sdr : 0.0;
    return max(sdr_base, 0.0) * scale;
}

float CombinedExposureFromPacked(float3 sdr_base, float3 hdr_src,
                                   float effective_peak_nits)
{
    // GainMapSplit applies the global and local exposure before BT.2446 and
    // stores the resulting SDR carrier. Recover that exact combined factor
    // from the carrier itself rather than re-running the local-exposure
    // estimator at compose time.
    const float3 pre_sdr = HdrPreDesaturate(max(sdr_base, 0.0), saturate(kCrosstalk));
    const float y_sdr = max(dot(pre_sdr, HdrLumaWeights()), 0.0);
    if (y_sdr <= 1e-8)
        return max(kExposure, 1e-6);

    const float y_curve = HdrBt2446Inverse(y_sdr * 100.0);
    const float y_exposed =
        y_curve * max(effective_peak_nits, kSdrReferenceWhiteNits) /
        kPqPeakNits;

    const float3 pre_hdr =
        HdrPreDesaturate(max(hdr_src, 0.0), saturate(kCrosstalk));
    const float y_hdr = max(dot(pre_hdr, HdrLumaWeights()), 0.0);
    if (y_hdr <= 1e-8)
        return max(kExposure, 1e-6);

    return max(y_exposed / y_hdr, 1e-6);
}

float3 EncodeHdr(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    float3 result = max(lin, 0.0);
    if (fmt == 1) {
        // The gain-map working domain uses 100-nit SDR white; scRGB encodes
        // scene-referred 1.0 as 80 nits. Convert back at the boundary.
        result = lin * (kSdrReferenceWhiteNits / kScRgbReferenceWhiteNits);
    }
    else if (fmt == 2) {
        result = NitsToPq(max(lin, 0.0) * kSdrReferenceWhiteNits);
    }
    return result;
}

// Inverse-tone-map shader reconstructs the model output as a scene-referred HDR
// target. The model exposure is recovered from the actual packed carrier, so it
// uses the same local SDR exposure scale that created the model input.
float3 ComposeHdrInverse(float3 sdr_base, float gain,
                         float3 low_in, float3 low_out, float local_sdr_scale)
{
    float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);
    if (kGradeEnabled > 0.5)
        low_out_lin = max(GradeLinearInv(low_out_lin), 0.0);

    const float3 model_sdr_base = GamutFitModelInput(sdr_base);
    const float3 kL = HdrLumaWeights();
    const float base_luma = dot(max(model_sdr_base, 0.0), kL);
    const float3 hdr_src = ReconstructHdr(sdr_base, gain);

    // Reconstruct both model-domain endpoints in scene-referred HDR using the
    // exact exposure that created the model input. The edit is the model
    // output minus model input, not model output minus the original HDR. That
    // distinction preserves high-frequency source detail when Working Scale
    // is below 1.0 while still transferring the model's scene-referred edit.
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float local_scale = max(local_sdr_scale, 1e-6);
    const float3 low_in_linear =
        GamutUnfitModelInput(max(SrgbToLinearExt(low_in), 0.0)) / local_scale;
    const float3 low_out_linear =
        GamutUnfitModelInput(max(low_out_lin, 0.0)) / local_scale;
    const float3 hdr_model_in = HdrFromLinearSdrCarrier(
        low_in_linear, kExposure, effective_peak_nits);
    const float3 hdr_model_out = HdrFromLinearSdrCarrier(
        low_out_linear, kExposure, effective_peak_nits);

    // Normalize the scene-referred model edit by the model-input HDR signal.
    // Exposure has already been removed from both endpoints, so the edit does
    // not shrink merely because the model had to be strongly exposed. The
    // delta is then applied to the original full-resolution HDR, retaining its
    // detail that the lower-resolution model could not represent.
    const float3 hdr_delta = hdr_model_out - hdr_model_in;
    const float3 delta_scale = max(abs(hdr_model_in), 0.001.xxx);
    const float3 normalized_delta = hdr_delta / delta_scale;
    const float3 shaped = ShapeResidual(normalized_delta, base_luma);
    const float3 hdr_edited = max(hdr_src + shaped * delta_scale, 0.0);

    return BlendYCbCr(hdr_src, hdr_edited, kLumaStrength, kChromaStrength);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);

    float4 packed = gOriginal.SampleLevel(s0, uv, 0);

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
    const float local_sdr_scale =
        kLocalExposureStrength > 0.0 && kEffectiveExposureMapValid > 0.5
            ? max(gEffectiveExposure.Load(int3(id.xy, 0)), 1e-6)
            : 1.0;
    const float3 final_lin = ComposeHdrInverse(
        sdr_base, packed.a, low_in, low_out, local_sdr_scale);
    const float3 final_out = EncodeHdr(final_lin);

    // Packed resources are linear gain-map data in the 100-nit normalized domain;
    // the destination is the game's original HDR encoding. Alpha is preserved
    // from the packed path only when
    // the target format conceptually carries it; for the game resource the
    // color channels are the only meaningful data.
    gOutput[id.xy] = float4(final_out, 1.0);
}

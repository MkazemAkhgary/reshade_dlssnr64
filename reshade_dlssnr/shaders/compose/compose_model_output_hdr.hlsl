#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"
#include "common/compose_common.hlsli"

Texture2D<float4>   gOriginal    : register(t0);
Texture2D<float4>   gLowInput    : register(t1);
Texture2D<float4>   gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput      : register(u0);
SamplerState        s0           : register(s0);

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

float3 EncodeHdr(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    float3 result = max(lin, 0.0);
    if (fmt == 1) {
        // Gain-map HDR is normalized to 100 nits; scRGB uses 80 nits/unit.
        result = lin * (100.0 / 80.0);
    }
    else if (fmt == 2) {
        result = NitsToPq(max(lin, 0.0) * 100.0);
    }
    return result;
}

// `kExposure` is retained in the uniform contract because the exposure EMA
// feeds the effective scene white on the CPU. The split and compose never
// multiply the image by it a second time.
float3 ComposeHdr(float3 sdr_base, float gain,
                  float3 low_in, float3 low_out)
{
    // The model interface is sRGB-encoded SDR. Decode after the model's
    // working-scale resample/sharpening, then undo the optional input grade so
    // the residual is measured in the same linear SDR domain as the gain map.
    float3 low_in_lin = max(SrgbToLinearExt(low_in), 0.0);
    float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);
    if (kGradeEnabled > 0.5) {
        low_in_lin = max(GradeLinearInv(low_in_lin), 0.0);
    }

    // The model sees a hue-preserving gamut fit of the carrier, not the carrier
    // itself. Those two are colinear per pixel, so a multiplicative model
    // residual measured in the model domain transfers directly back to the
    // lossless carrier. The carrier itself remains unclipped.
    const float3 model_sdr_base = GamutFitPreserveChroma(sdr_base);
    const float3 kL = HdrLumaWeights();
    const float base_luma = dot(max(model_sdr_base, 0.0), kL);
    const float3 delta = ShapeResidual(
        ComposeRatioDelta(low_in_lin, low_out_lin), base_luma);

    // Decode once with the ISO offsets to get the true HDR source.
    const float3 hdr_src = ReconstructHdr(sdr_base, gain);

    // Apply the model's multiplicative edit directly to HDR. Do NOT feed
    // sdr_base * (1 + delta) back through ReconstructHdr: the affine
    // -kAlternateOffset term adds a*(delta) of absolute error and drives
    // highlights to black. The ratio delta is dimensionless and applies
    // identically in the SDR model domain and the HDR reconstruction
    // domain, because both are derived from the same carrier pixel.
    const float3 hdr_edited = max(hdr_src * (1.0 + delta), 0.0);

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
    const float3 final_lin = ComposeHdr(sdr_base, packed.a, low_in, low_out);
    const float3 final_out = EncodeHdr(final_lin);

    // Packed resources are linear gain-map data in the 100-nit normalized domain;
    // the destination is the game's original HDR encoding. Alpha is preserved
    // from the packed path only when
    // the target format conceptually carries it; for the game resource the
    // color channels are the only meaningful data.
    gOutput[id.xy] = float4(final_out, 1.0);
}

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

#include "common/fsr.hlsli"

float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}


float3 EncodeHdr(float3 lin)
{
    return HdrEncodeTarget(lin, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
}

// `kExposure` is retained in the uniform contract because the exposure EMA
// feeds the effective scene white on the CPU. The split and compose never
// multiply the image by it a second time.
float3 ShapeHdrTargetDelta(float3 delta, float luma)
{
    if (kSoftCompression != 0) {
        delta.r = SoftCompress(delta.r, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        delta.g = SoftCompress(delta.g, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        delta.b = SoftCompress(delta.b, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
    }

    if (kEditGamma != 1.0 && kEditGamma > 0.0) {
        delta = sign(delta) * pow(abs(delta), kEditGamma);
    }

    if (kResidualChroma < 1.0) {
        const float3 kL = float3(0.2126, 0.7152, 0.0722);
        float d_luma = dot(delta, kL);
        delta = lerp(float3(d_luma, d_luma, d_luma), delta, kResidualChroma);
    }

    float protect = 1.0;
    if (kHighlightProtect > 0.0) {
        float h = saturate((luma - 0.7) / 0.3);
        protect *= 1.0 - kHighlightProtect * h;
    }
    if (kShadowProtect > 0.0) {
        float s = saturate((0.3 - luma) / 0.3);
        protect *= 1.0 - kShadowProtect * s;
    }

    return delta * protect * kEditStrength;
}

float3 ComposeHdr(float3 sdr_base, float3 hdr_src,
                  float3 low_in, float3 low_out)
{
    float3 low_in_lin = max(SrgbToLinearExt(low_in), 0.0);
    float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);
    if (kGradeEnabled > 0.5)
        low_in_lin = max(GradeLinearInv(low_in_lin), 0.0);

    const float3 model_sdr_base = GamutFitModelInput(sdr_base);
    const float3 kL = HdrLumaWeights();
    const float base_luma = dot(max(model_sdr_base, 0.0), kL);
    const float3 delta = ShapeResidual(
        ComposeRatioDelta(low_in_lin, low_out_lin), base_luma);
    if (all(delta == 0.0))
        return hdr_src;
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
    const float3 source_encoded = gHdrSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_src = DecodeToLinear(
        source_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    const float3 final_lin = ComposeHdr(sdr_base, hdr_src, low_in, low_out);
    const float3 encoded_result = EncodeHdr(final_lin);
    // Preserve the exact native-format code for unchanged channels. This avoids
    // decode/encode round-off moving an untouched FP16/PQ channel by one code.
    const float3 final_out = float3(
        final_lin.r == hdr_src.r ? source_encoded.r : encoded_result.r,
        final_lin.g == hdr_src.g ? source_encoded.g : encoded_result.g,
        final_lin.b == hdr_src.b ? source_encoded.b : encoded_result.b);
    gOutput[id.xy] = float4(final_out, 1.0);
}

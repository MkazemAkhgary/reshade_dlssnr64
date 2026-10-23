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

#include "common/compose_upsample.hlsli"

// Shared shader state is declared by the common cbuffer/bindings includes.

// Same shaping as ShapeResidual in the HDR compose, but on the SDR path's
// log-space residual rather than a per-channel ratio. Separate functions
// because the input spaces differ and the region protection reads a
// different luma; the sliders are the same.
float3 ShapeResidualSdr(float3 delta, float3 original_srgb) {
    // When kSoftCompression is enabled, the SDR caller already applies the
    // luma/chroma-specific soft compression once before this function.
    if (kEditGamma != 1.0 && kEditGamma > 0.0) {
        delta = sign(delta) * pow(abs(delta), kEditGamma);
    }

    if (kResidualChroma < 1.0) {
        const float3 kL = float3(0.2126, 0.7152, 0.0722);
        float d_luma = dot(delta, kL);
        delta = lerp(float3(d_luma, d_luma, d_luma), delta, kResidualChroma);
    }

    const float3 kL = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(original_srgb, kL);
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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);

    float4 original = gOriginal.SampleLevel(s0, uv, 0);

    // Reconstruct both model-domain images with the fixed joint-bilateral 3x3 filter.
    float3 in_rgb = float3(0.0, 0.0, 0.0);
    float3 out_rgb = float3(0.0, 0.0, 0.0);
    uint low_w = 0u;
    uint low_h = 0u;
    gLowInput.GetDimensions(low_w, low_h);
    if (low_w == Width && low_h == Height) {
        in_rgb = gLowInput.Load(int3(id.xy, 0)).rgb;
        out_rgb = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
    } else {
        const int2 full_size_int = int2((int)Width, (int)Height);
        const int2 full_xy = int2((int)id.x, (int)id.y);
        JointBilateralUpsamplePair(
            full_xy, full_size_int, gLowInput, gLowEnhanced, gOriginal,
            kUpsampleSharpness, kUpsampleSharpness, in_rgb, out_rgb);
    }

    // The model's edit as a per-channel ratio, held in log space so the
    // compression sliders can bound it on both sides.
    float3 residual_log;
    if (kSoftCompression != 0) {
        float in_luma = max(dot(in_rgb, float3(0.2126, 0.7152, 0.0722)), 1e-6);
        float out_luma = max(dot(out_rgb, float3(0.2126, 0.7152, 0.0722)), 1e-6);

        float residual_luma = safe_log(out_luma) - safe_log(in_luma);
        float3 residual_chroma =
            safe_log3(out_rgb) - safe_log(out_luma) -
            safe_log3(in_rgb) + safe_log(in_luma);

        residual_luma = SoftCompress(
            residual_luma, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        residual_chroma.x = SoftCompress(
            residual_chroma.x, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);
        residual_chroma.y = SoftCompress(
            residual_chroma.y, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);
        residual_chroma.z = SoftCompress(
            residual_chroma.z, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);

        residual_log = residual_luma + residual_chroma;
    } else {
        residual_log = safe_log3(out_rgb) - safe_log3(in_rgb);
    }

    residual_log = clamp(residual_log,
                         log(max(kRatioDarken, 1e-4)),
                         log(max(kRatioBrighten, 1e-4)));
    // Region protection has to be evaluated in the domain the edit was measured
    // in. With kApplyEditToPreGrade the endpoints are the graded image while
    // the base is the pre-grade one, so the base's luma would protect the wrong
    // regions and the edit would land at the wrong strength there.
    //
    // The residual stays a ratio in the graded domain on purpose. The model
    // reconstructs a plausible image, so its edit has to land as the model
    // produced it; inverting the grade first would move the result away from
    // that intent, not toward it.
    const float3 protection_rgb = kApplyEditToPreGrade > 0.5 ? in_rgb : original.rgb;
    const float3 residual_shaped = ShapeResidualSdr(residual_log, protection_rgb);
    float3 edited = original.rgb * exp(residual_shaped);
    float3 final_rgb = BlendYCbCr(original.rgb, edited,
                                  kLumaStrength, kChromaStrength);
    final_rgb = max(final_rgb, 0.0);
    gOutput[id.xy] = float4(final_rgb, original.a);
}

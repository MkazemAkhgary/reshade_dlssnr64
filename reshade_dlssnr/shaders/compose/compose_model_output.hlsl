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

// ---------------------------------------------------------------------------
// Joint bilateral upsampling, guided by a full-resolution image.
//
// Each full-resolution output pixel is the weighted average of the 3x3
// low-resolution samples nearest its projected low-res coordinate. The
// weight of a low-res sample is exp(-|dG| * sharpness), where dG is the
// difference between the guide luma at that sample's corresponding
// full-resolution position and the guide luma at the output pixel.
//
// At working scale 1.0 the caller reads the low-res texture directly, so
// this helper is only reached when the projection spans multiple source
// pixels.
// ---------------------------------------------------------------------------
float3 JointBilateralUpsample(
    int2 full_xy,
    int2 full_size,
    Texture2D<float4> low_tex,
    Texture2D<float4> guide_tex,
    float sharpness)
{
    uint low_w_u = 0u;
    uint low_h_u = 0u;
    low_tex.GetDimensions(low_w_u, low_h_u);
    const int low_w = max((int)low_w_u, 1);
    const int low_h = max((int)low_h_u, 1);
    if (full_size.x <= 0 || full_size.y <= 0) return float3(0.0, 0.0, 0.0);

    const float2 full_sz = float2((float)full_size.x, (float)full_size.y);
    const float2 low_sz  = float2((float)low_w, (float)low_h);

    // Pixel-center projection from full-res to low-res space. At 1:1 this
    // lands exactly on the source pixel; at other scales it interpolates.
    const float2 full_center = float2((float)full_xy.x + 0.5,
                                      (float)full_xy.y + 0.5);
    const float2 low_pix = full_center * (low_sz / full_sz);
    const int2 low_center = int2((int)floor(low_pix.x),
                                 (int)floor(low_pix.y));

    const float3 guide_center = guide_tex.Load(int3(full_xy, 0)).rgb;
    const float guide_luma_center =
        dot(guide_center, float3(0.2126, 0.7152, 0.0722));

    float3 accum = float3(0.0, 0.0, 0.0);
    float  weight_sum = 0.0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 low_xy = int2(low_center.x + dx, low_center.y + dy);
            if (low_xy.x < 0 || low_xy.y < 0 ||
                low_xy.x >= low_w || low_xy.y >= low_h) {
                continue;
            }

            // Full-res position of this low-res sample's center, so the
            // guide is read at the same feature the sample represents.
            const float2 low_neighbor_uv = float2(
                ((float)low_xy.x + 0.5) / (float)low_w,
                ((float)low_xy.y + 0.5) / (float)low_h);
            int2 full_neighbor_xy = int2(
                (int)floor(low_neighbor_uv.x * full_sz.x),
                (int)floor(low_neighbor_uv.y * full_sz.y));
            full_neighbor_xy.x =
                clamp(full_neighbor_xy.x, 0, full_size.x - 1);
            full_neighbor_xy.y =
                clamp(full_neighbor_xy.y, 0, full_size.y - 1);

            const float3 guide_neighbor =
                guide_tex.Load(int3(full_neighbor_xy, 0)).rgb;
            const float guide_luma_neighbor =
                dot(guide_neighbor, float3(0.2126, 0.7152, 0.0722));

            const float luma_diff =
                abs(guide_luma_neighbor - guide_luma_center);
            const float weight = exp(-luma_diff * sharpness);

            const float3 low_val = low_tex.Load(int3(low_xy, 0)).rgb;
            accum += low_val * weight;
            weight_sum += weight;
        }
    }

    if (weight_sum > 0.0) return accum / weight_sum;

    // Degenerate weight sum: fall back to the projected sample directly.
    const int2 clamped_center = int2(
        clamp(low_center.x, 0, low_w - 1),
        clamp(low_center.y, 0, low_h - 1));
    return low_tex.Load(int3(clamped_center, 0)).rgb;
}


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

    // FSR 1.0 EASU upsample from the actual model working resolution back to
    // full SDR resolution. Both HDR and SDR compose use the same reconstruction.
    float3 in_rgb;
    float3 out_rgb;
    if (kEasuCon0.x == 1.0 && kEasuCon0.y == 1.0) {
        // Working scale 1:1: the low-res buffers are already full resolution,
        // so read them directly.
        in_rgb = gLowInput.Load(int3(id.xy, 0)).rgb;
        out_rgb = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
    } else {
        // Joint bilateral upsample the model input and output separately,
        // guided by the full-resolution original. This replaces the EASU
        // reconstruction, which produces a blocky pattern when the working
        // scale is far from EASU's design point (~2x). The residual math
        // below is unchanged: it still operates on in_rgb and out_rgb, so
        // the model's per-channel edit — chroma included — is preserved.
        const int2 full_size_int = int2((int)Width, (int)Height);
        const int2 full_xy = int2((int)id.x, (int)id.y);
        in_rgb  = JointBilateralUpsample(
            full_xy, full_size_int, gLowInput,  gOriginal, 8.0);
        out_rgb = JointBilateralUpsample(
            full_xy, full_size_int, gLowEnhanced, gOriginal, 8.0);
    }

    if (kRcasEnabled > 0.5) {
        const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);

        float3 b_in = gLowInput.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_in = gLowInput.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_in = gLowInput.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_in = gLowInput.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        in_rgb = FsrRcasF(b_in, d_in, in_rgb, f_in, h_in, kRcasStrength);

        float3 b_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_out = gLowEnhanced.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_out = gLowEnhanced.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        out_rgb = FsrRcasF(b_out, d_out, out_rgb, f_out, h_out, kRcasStrength);
    }

    // The model's edit as a per-channel ratio, held in log space so the
    // compression sliders can bound it on both sides of 1.0.
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

    const float3 residual_shaped = ShapeResidualSdr(residual_log, original.rgb);

    // When an input grade ran, apply it before the ratio and invert after.
    const bool graded = (kGradeEnabled > 0.5);

    float3 edited;
    if (graded) {
        // The SDR path has no tonemap; the model's domain is the sRGB-encoded
        // image it saw, so the grade and the ratio act on that encoding
        // directly.
        float3 graded_original = GradeLinearFwd(original.rgb);
        float3 graded_edited = graded_original * exp(residual_shaped);
        edited = GradeLinearInv(graded_edited);
    } else {
        edited = original.rgb * exp(residual_shaped);
    }

    float3 final_rgb = BlendYCbCr(original.rgb, edited,
                                  kLumaStrength, kChromaStrength);
    final_rgb = max(final_rgb, 0.0);

    gOutput[id.xy] = float4(final_rgb, original.a);
}

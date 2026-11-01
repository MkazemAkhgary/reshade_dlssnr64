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

#include "common/compose_upsample.hlsli"

// Shared shader state is declared by the common cbuffer/bindings includes.

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    // 1:1 with the dispatch, so a filtered fetch would only add sampler work.
    const float4 original = gOriginal.Load(int3(id.xy, 0));

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

    // The residual is a ratio of linear light and the base is multiplied in
    // linear light, which is what the HDR compose does. Applying the ratio to
    // the encoded values instead is exact only where the full-resolution base
    // equals the reference; everywhere the base carries detail the reference
    // was averaged over, the transfer curve leaves a residue, and a lift
    // multiplies that residue by its own size.
    const float3 base_lin = SrgbToLinearExt(original.rgb);
    const float3 in_lin = max(SrgbToLinearExt(in_rgb), 0.0);
    const float3 out_lin = max(SrgbToLinearExt(out_rgb), 0.0);

    // The model's edit as a per-channel ratio, held in log space so the
    // compression sliders can bound it on both sides.
    float3 residual_log = safe_log3(out_lin) - safe_log3(in_lin);

    if (kRatioClampsEnabled) {
        residual_log = clamp(residual_log,
                             log(max(kRatioDarken, 1e-4)),
                             log(max(kRatioBrighten, 1e-4)));
    }
    // Region protection has to be evaluated in the domain the edit was measured
    // in. With kApplyEditToPreGrade the endpoints are the graded image while
    // the base is the pre-grade one, so the base's luma would protect the wrong
    // regions and the edit would land at the wrong strength there.
    //
    // The residual stays a ratio in the graded domain on purpose. The model
    // reconstructs a plausible image, so its edit has to land as the model
    // produced it; inverting the grade first would move the result away from
    // that intent, not toward it.
    const float3 protection_rgb = kApplyEditToPreGrade > 0.5 ? in_lin : base_lin;
    const float3 residual_shaped = ShapeResidual(
        residual_log, dot(protection_rgb, float3(0.2126, 0.7152, 0.0722)));
    float3 edited = base_lin * exp(residual_shaped);
    float3 final_lin = BlendYCbCr(base_lin, edited,
                                  kLumaStrength, kChromaStrength);
    gOutput[id.xy] = float4(LinearToSrgb(max(final_lin, 0.0)), original.a);
}

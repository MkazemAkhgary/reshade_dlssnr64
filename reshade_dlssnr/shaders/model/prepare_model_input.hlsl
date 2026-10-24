#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/box_filter.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gGlobalCarrier : register(t1);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    // Local-exposure gain at working resolution: lifted carrier over global
    // carrier. The HDR pre-grade compose reconstructs this instead of
    // re-evaluating the gain per full-resolution pixel, so the lift stays free
    // of full-resolution contrast. Both carriers are linear and untransformed,
    // so the ratio is the linear gain the normal compose path gets from its
    // residual; taking it from the model inputs instead would go through the
    // gamut fit and the sRGB encode and flatten the lift.
    if (shader.Params0.z != 0.0)
    {
        uint lifted_w = 0u;
        uint lifted_h = 0u;
        uint global_w = 0u;
        uint global_h = 0u;
        gSource.GetDimensions(lifted_w, lifted_h);
        gGlobalCarrier.GetDimensions(global_w, global_h);
        const float3 lifted = BoxAreaAverage(
            gSource, id.xy, uint2(Width, Height), uint2(lifted_w, lifted_h)).rgb;
        const float3 global = BoxAreaAverage(
            gGlobalCarrier, id.xy, uint2(Width, Height),
            uint2(global_w, global_h)).rgb;
        const float3 gain = lifted / max(global, 1e-5);
        gOutput[id.xy] = float4(clamp(gain, 1.0 / 256.0, 256.0), 1.0);
        return;
    }

    // Exact area average over the source footprint covered by this working
    // pixel. This is the downsampling filter for both SDR and HDR carrier
    // inputs; it does not depend on the global linear-clamp sampler.
    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    float4 color = BoxAreaAverage(gSource, id.xy, uint2(Width, Height),
                                  uint2(src_width, src_height));

    float3 result;
    if (shader.Params0.w != 0.0)
    {
        // HDR gain-map sources are a lossless linear SDR carrier and may contain
        // super-whites. Compress only the bright tail into the model gamut while
        // retaining highlight color/intensity information; HDR compose inverts the
        // same transform before reconstructing the model-output HDR target. The
        // packed carrier remains untouched and is what HDR compose reconstructs from.
        float3 linearColor = color.rgb;
        linearColor = GamutFitModelInput(linearColor);
        result = LinearToSrgb(linearColor);
    }
    else
    {
        // SDR path: the source is already in the target (sRGB) space, so pass
        // it through without an extra transfer-function conversion.
        result = color.rgb;
    }

    gOutput[id.xy] = float4(result, 1.0);
}

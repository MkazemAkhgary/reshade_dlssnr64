#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"

Texture2D<float4> gSource : register(t0);
Texture2D<float4> gLocalCarrier : register(t1);
Texture2D<float4> gGlobalCarrier : register(t2);
Texture2D<float> gGlobalScale : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);


float3 DecodeSource(float3 c)
{
    int fmt = (int)(kGradeFormat + 0.5);
    float3 result = SrgbToLinearExt(c);

    if (fmt == 1)
        result = max(c, 0.0) *
                 (kScRgbReferenceWhiteNits / kSdrReferenceWhiteNits);
    else if (fmt == 2)
        result = PqToNits(c) / kSdrReferenceWhiteNits;
    else if (fmt == 3)
        result = c;

    return result;
}

float3 EncodeSource(float3 lin)
{
    int fmt = (int)(kGradeFormat + 0.5);
    float3 safe = max(lin, 0.0);
    const float peak = max(safe.r, max(safe.g, safe.b));
    // Model-input SDR is gamut limited, but clipping each channel independently
    // changes hue/chroma. Scale the complete RGB vector when it exceeds the
    // boundary so its chromaticity remains unchanged.
    if (peak > 1.0)
        safe *= 1.0 / peak;
    float3 result = LinearToSrgbExt(safe);

    if (fmt == 1)
        result = max(lin, 0.0) *
                 (kSdrReferenceWhiteNits / kScRgbReferenceWhiteNits);
    else if (fmt == 2)
        result = NitsToPq(max(lin, 0.0) * kSdrReferenceWhiteNits);
    else if (fmt == 3)
        result = lin;

    return result;
}

float3 RestoreExposureAt(uint2 pixel, float3 linear_rgb)
{
    if (kRestoreExposureFlag < 0.0)
        return linear_rgb;

    float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
    const float local_inverse_scale = gLocalCarrier.SampleLevel(s0, uv, 0).a;
    const float global_inverse_scale = gGlobalCarrier.SampleLevel(s0, uv, 0).a;
    const float local_scale = max(
        global_inverse_scale / max(local_inverse_scale, 1.0e-8), 1.0e-6);
    const float mode = saturate(kRestoreExposureFlag);
    float restore_scale = local_scale;
    if (mode > 0.0) {
        const float global_scale = max(gGlobalScale.Load(int3(0, 0, 0)), 1.0e-6);
        restore_scale = exp2(lerp(log2(local_scale), log2(global_scale), mode));
    }
    const float restore_strength = saturate(hdr.LocalExposureParams.w);
    restore_scale = exp2(log2(max(restore_scale, 1.0e-6)) * restore_strength);
    return linear_rgb / restore_scale;
}

float3 ReadLinearSource(uint2 pixel)
{
    const float3 sampled = gSource.Load(int3(pixel, 0)).rgb;
    return RestoreExposureAt(pixel, DecodeSource(sampled));
}

float3 EdgeAwareSoftened(uint2 pixel, float3 center)
{
    if (kEdgeSofteningEnabled < 0.5 || kEdgeSoftening <= 0.0)
        return center;

    const float center_luma = dot(center, float3(0.2126, 0.7152, 0.0722));
    float3 weighted_sum = center;
    float weight_sum = 1.0;
    [unroll]
    for (int oy = -1; oy <= 1; ++oy) {
        [unroll]
        for (int ox = -1; ox <= 1; ++ox) {
            if (ox == 0 && oy == 0)
                continue;
            const int2 q = clamp(int2(pixel) + int2(ox, oy), int2(0, 0),
                                 int2(Width, Height) - 1);
            const float3 neighbor = ReadLinearSource(uint2(q));
            const float neighbor_luma = dot(neighbor, float3(0.2126, 0.7152, 0.0722));
            const float spatial = (ox == 0 || oy == 0) ? 2.0 : 1.0;
            const float range_weight = rcp(1.0 + abs(neighbor_luma - center_luma) * 32.0);
            const float weight = spatial * range_weight;
            weighted_sum += neighbor * weight;
            weight_sum += weight;
        }
    }
    const float3 softened = weighted_sum / max(weight_sum, 1.0e-6);
    return lerp(center, softened, saturate(kEdgeSoftening));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    const uint2 pixel = id.xy;
    const float4 src = gSource.Load(int3(pixel, 0));
    float3 linear_rgb = ReadLinearSource(pixel);
    linear_rgb = EdgeAwareSoftened(pixel, linear_rgb);
    const float3 graded = GradeLinearFwd(linear_rgb);
    gOutput[pixel] = float4(EncodeSource(graded), src.a);
}

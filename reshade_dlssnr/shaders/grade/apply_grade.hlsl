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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv =
        (float2(id.xy) + 0.5) /
        float2(Width, Height);

    float4 src =
        gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeSource(src.rgb);

    // Negative means restoration is disabled. Zero selects the exact local
    // multiplier; one selects a uniform global multiplier. Interpolation is
    // performed in stops so both ends remain perceptually meaningful.
    if (kRestoreExposureFlag >= 0.0) {
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
        lin /= restore_scale;
    }

    float3 graded = GradeLinearFwd(lin);

    gOutput[id.xy] =
        float4(EncodeSource(graded), src.a);
}

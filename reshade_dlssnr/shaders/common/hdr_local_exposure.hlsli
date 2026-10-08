#ifndef DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI
#define DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI

// Shared local-exposure calculation used by both GainMapSplit and the
// full-resolution exposure map. Keep this in one place so the exposure field
// handed to inverse HDR composition is exactly the field that created the
// model input.

float SampleHdrLumaForLocalExposure(float2 uv)
{
    float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 lin = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    lin = HdrPreDesaturate(lin, saturate(kCrosstalk));
    return max(dot(max(lin, 0.0), HdrLumaWeights()), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2, float2 texel)
{
    const float min_dim = min((float)Width, (float)Height);
    const float radius = max(1.0, min_dim * 0.01);
    const float2 r = texel * radius;

    float weighted = center_log2 * 4.0;
    float weight = 4.0;

    float l = log2(SampleHdrLumaForLocalExposure(uv + float2(-r.x, 0.0)));
    float w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w;
    weight += w;

    l = log2(SampleHdrLumaForLocalExposure(uv + float2(r.x, 0.0)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w;
    weight += w;

    l = log2(SampleHdrLumaForLocalExposure(uv + float2(0.0, -r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w;
    weight += w;

    l = log2(SampleHdrLumaForLocalExposure(uv + float2(0.0, r.y)));
    w = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
    weighted += l * w;
    weight += w;

    return weighted / max(weight, 1e-6);
}

float LocalExposureFactor(float2 uv, float raw_luma, float exposure)
{
    if (kLocalExposureStrength <= 0.0 || raw_luma <= 1e-8)
        return 1.0;

    const float2 texel = 1.0 / float2(max(Width, 1u), max(Height, 1u));
    const float center_log2 = log2(raw_luma);
    const float local_base_log2 = LocalBaseLogLuma(uv, center_log2, texel);
    const float target_base_log2 =
        log2(max(kLocalExposureMidtone, 1e-6)) - log2(max(exposure, 1e-6));
    const float adjusted_base_log2 = lerp(
        local_base_log2, target_base_log2,
        saturate(kLocalExposureStrength));
    const float stops = clamp(
        adjusted_base_log2 - local_base_log2,
        -kLocalMaxDarken, kLocalMaxBrighten);
    return exp2(stops);
}

#endif

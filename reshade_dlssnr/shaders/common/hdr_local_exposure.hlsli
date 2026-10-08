#ifndef DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI
#define DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI

// Local exposure is deliberately applied in the linear SDR luminance domain
// after global eye adaptation and BT.2446C. The operator only changes a smooth
// base layer; the high-frequency log-luminance detail is carried through
// unchanged. This prevents local exposure from destroying texture/specular
// detail before the model sees it.

float GlobalHdrLumaToSdr(float hdr_luma, float exposure)
{
    const float y_exposed = max(hdr_luma, 0.0) * max(exposure, 1e-6);
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits,
        max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve =
        y_exposed * kPqPeakNits / effective_peak_nits;
    return max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
}

float SampleGlobalSdrLuma(float2 uv)
{
    float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 lin = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    lin = HdrPreDesaturate(lin, saturate(kCrosstalk));
    const float hdr_luma = max(
        dot(max(lin, 0.0), HdrLumaWeights()), 0.0);
    return max(GlobalHdrLumaToSdr(hdr_luma, kExposure), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2, float2 texel)
{
    const float min_dim = min((float)Width, (float)Height);
    const float radius = max(1.0, min_dim * 0.01);
    const float2 r = texel * radius;

    // Small bilateral base layer. Spatial weighting keeps the base smooth;
    // the log-luminance distance term keeps strong edges from bleeding across
    // objects, while the center/detail term remains untouched by the exposure
    // adjustment below.
    float weighted = center_log2 * 8.0;
    float weight = 8.0;

    const float2 offsets[8] = {
        float2(-1.0, 0.0), float2(1.0, 0.0),
        float2(0.0, -1.0), float2(0.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0, 1.0), float2(1.0, 1.0)};
    const float spatial_weights[8] = {
        1.0, 1.0, 1.0, 1.0,
        0.75, 0.75, 0.75, 0.75};

    [unroll]
    for (uint i = 0; i < 8u; ++i) {
        const float l = log2(SampleGlobalSdrLuma(uv + offsets[i] * r));
        const float spatial = spatial_weights[i];
        const float edge = 1.0 / (1.0 + 4.0 * abs(l - center_log2));
        const float w = spatial * edge;
        weighted += l * w;
        weight += w;
    }

    return weighted / max(weight, 1e-6);
}

float LocalExposureScale(float2 uv, float global_sdr_luma)
{
    if (kLocalExposureStrength <= 0.0 || global_sdr_luma <= 1e-8)
        return 1.0;

    const float2 texel = 1.0 / float2(max(Width, 1u), max(Height, 1u));
    const float center_log2 = log2(max(global_sdr_luma, 1e-8));
    const float base_log2 = LocalBaseLogLuma(uv, center_log2, texel);
    const float detail_log2 = center_log2 - base_log2;
    const float mid_log2 = log2(max(kLocalExposureMidtone, 1e-6));

    // Local Exposure is a base/detail operation. Compress the base toward
    // middle gray, then add the original local detail back unchanged in log
    // luminance. The resulting multiplicative scale is therefore derived from
    // the base layer only, not from the fine texture/high-frequency signal.
    const float distance_from_mid = base_log2 - mid_log2;
    const float shadow_weight =
        1.0 - smoothstep(-2.5, -0.5, distance_from_mid);
    const float highlight_weight =
        smoothstep(0.5, 2.5, distance_from_mid);
    const float region_weight = max(shadow_weight, highlight_weight);

    const float base_strength = saturate(kLocalExposureStrength) * region_weight;
    const float new_base_log2 = lerp(base_log2, mid_log2, base_strength);
    const float new_log2 = new_base_log2 + detail_log2;
    const float stops = clamp(
        new_log2 - center_log2,
        -kLocalMaxDarken,
        kLocalMaxBrighten);
    return exp2(stops);
}


#endif

#ifndef DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI
#define DLSSNR_HDR_LOCAL_EXPOSURE_HLSLI

#define kLocalFilterFlags ((uint)(shader.Params0.x + 0.5))
#define kLocalDepthNear shader.Params0.y
#define kLocalDepthFar shader.Params0.z
#define kLocalDepthInverted shader.Params0.w
#define kLocalLumaMin shader.Params1.x
#define kLocalLumaMax shader.Params1.y

float GlobalHdrLumaToSdr(float hdr_luma, float exposure)
{
    const float y_exposed = max(hdr_luma, 0.0) * max(exposure, 1e-6);
    const float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    const float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    return max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
}

float SampleGlobalSdrLuma(float2 uv)
{
    const float3 c = gSource.SampleLevel(s0, saturate(uv), 0).rgb;
    float3 linear_rgb = DecodeToLinear(
        c, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    linear_rgb = HdrPreDesaturate(linear_rgb, saturate(kCrosstalk));
    const float hdr_luma = max(dot(max(linear_rgb, 0.0), HdrLumaWeights()), 0.0);
    return max(GlobalHdrLumaToSdr(hdr_luma, kExposure), 1e-8);
}

float LocalBaseLogLuma(float2 uv, float center_log2, float2 texel)
{
    const float min_dim = min((float)Width, (float)Height);
    const float radius = max(1.0, min_dim * 0.01);
    const float2 r = texel * radius;
    float weighted = center_log2 * 8.0;
    float weight = 8.0;
    const float2 offsets[8] = {
        float2(-1.0, 0.0), float2(1.0, 0.0),
        float2(0.0, -1.0), float2(0.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, -1.0),
        float2(-1.0, 1.0), float2(1.0, 1.0)};
    const float spatial_weights[8] = {
        1.0, 1.0, 1.0, 1.0, 0.75, 0.75, 0.75, 0.75};

    [unroll]
    for (uint i = 0; i < 8u; ++i) {
        const float local_luma = log2(SampleGlobalSdrLuma(uv + offsets[i] * r));
        const float edge = 1.0 / (1.0 + 4.0 * abs(local_luma - center_log2));
        const float w = spatial_weights[i] * edge;
        weighted += local_luma * w;
        weight += w;
    }
    return weighted / max(weight, 1e-6);
}

float LocalFilterWeight(float2 uv, float source_hdr_luma)
{
    float weight = 1.0;
    if ((kLocalFilterFlags & 1u) != 0u) {
        uint mask_width, mask_height;
        gMask.GetDimensions(mask_width, mask_height);
        const uint2 mask_xy = min(
            uint2(saturate(uv) * float2(mask_width, mask_height)),
            uint2(mask_width - 1u, mask_height - 1u));
        weight *= saturate(gMask.Load(int3(mask_xy, 0)));
    }
    if ((kLocalFilterFlags & 2u) != 0u) {
        uint depth_width, depth_height;
        gDepth.GetDimensions(depth_width, depth_height);
        const uint2 depth_xy = min(
            uint2(saturate(uv) * float2(depth_width, depth_height)),
            uint2(depth_width - 1u, depth_height - 1u));
        float depth = saturate(gDepth.Load(int3(depth_xy, 0)));
        if (kLocalDepthInverted > 0.5) depth = 1.0 - depth;
        if (depth < kLocalDepthNear || depth > kLocalDepthFar)
            weight = 0.0;
    }
    if ((kLocalFilterFlags & 4u) != 0u) {
        const float lower_edge = max(kLocalLumaMin, 1e-8);
        const float upper_edge = max(kLocalLumaMax, lower_edge + 1e-6);
        const float lower_width = max(lower_edge * 0.08, 1e-6);
        const float upper_width = max(upper_edge * 0.08, 1e-6);
        const float low_weight = smoothstep(
            max(lower_edge - lower_width, 0.0), lower_edge + lower_width,
            max(source_hdr_luma, 0.0));
        const float high_weight = 1.0 - smoothstep(
            max(upper_edge - upper_width, lower_edge), upper_edge + upper_width,
            max(source_hdr_luma, 0.0));
        weight *= saturate(low_weight * high_weight);
    }
    return saturate(weight);
}

float LocalExposureScale(float2 uv, float global_sdr_luma,
                         float source_hdr_luma)
{
    if (kLocalExposureStrength <= 0.0 || global_sdr_luma <= 1e-8)
        return 1.0;

    const float2 texel = 1.0 / float2(max(Width, 1u), max(Height, 1u));
    const float center_log2 = log2(max(global_sdr_luma, 1e-8));
    const float base_log2 = LocalBaseLogLuma(uv, center_log2, texel);
    const float detail_log2 = center_log2 - base_log2;
    const float mid_log2 = log2(max(kLocalExposureMidtone, 1e-6));
    const float distance_from_mid = base_log2 - mid_log2;
    const float shadow_weight = 1.0 - smoothstep(-2.5, -0.5, distance_from_mid);
    const float highlight_weight = smoothstep(0.5, 2.5, distance_from_mid);
    const float region_weight = max(shadow_weight, highlight_weight);
    const float strength = saturate(kLocalExposureStrength) * region_weight;
    const float new_base_log2 = lerp(base_log2, mid_log2, strength);
    const float new_log2 = new_base_log2 + detail_log2;
    const float stops = clamp(new_log2 - center_log2,
                              -kLocalMaxDarken, kLocalMaxBrighten);
    const float scale = exp2(stops);
    return lerp(1.0, scale, LocalFilterWeight(uv, source_hdr_luma));
}

#endif

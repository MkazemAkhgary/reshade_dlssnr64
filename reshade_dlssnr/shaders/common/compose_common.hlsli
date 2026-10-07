#ifndef DLSSNR_COMPOSE_COMMON_HLSLI
#define DLSSNR_COMPOSE_COMMON_HLSLI

float3 BlendYCbCr(float3 original, float3 edited,
                  float luma_strength, float chroma_strength)
{
    const float3 kL = HdrLumaWeights();
    const float wr = kL.r;
    const float wg = max(kL.g, 1e-6);
    const float wb = kL.b;

    // Derive the normalized chroma basis from the active luma weights rather
    // than hard-coding BT.709 coefficients. This keeps HDR10/BT.2020 and
    // scRGB/BT.709 reconstruction colorimetrically self-consistent.
    const float cb_scale = 2.0 * (1.0 - wb);
    const float cr_scale = 2.0 * (1.0 - wr);
    const float g_cb = 2.0 * wb * (1.0 - wb) / wg;
    const float g_cr = 2.0 * wr * (1.0 - wr) / wg;

    float orig_y = dot(original, kL);
    float edit_y = dot(edited, kL);
    float final_y = lerp(orig_y, edit_y, luma_strength);

    float safe_orig_y = max(orig_y, 1e-4);
    float safe_edit_y = max(edit_y, 1e-4);
    float safe_final_y = max(final_y, 1e-4);

    float orig_cbn = (original.b - orig_y) / (cb_scale * safe_orig_y);
    float orig_crn = (original.r - orig_y) / (cr_scale * safe_orig_y);
    float edit_cbn = (edited.b - edit_y) / (cb_scale * safe_edit_y);
    float edit_crn = (edited.r - edit_y) / (cr_scale * safe_edit_y);

    float final_cbn = lerp(orig_cbn, edit_cbn, chroma_strength);
    float final_crn = lerp(orig_crn, edit_crn, chroma_strength);

    float final_cb = final_cbn * safe_final_y;
    float final_cr = final_crn * safe_final_y;

    float3 result;
    result.r = final_y + cr_scale * final_cr;
    result.b = final_y + cb_scale * final_cb;
    result.g = final_y - g_cb * final_cb - g_cr * final_cr;
    return result;
}



float3 ShapeResidual(float3 delta, float luma) {
    if (kSoftCompression != 0) {
        delta.r = SoftCompress(delta.r, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        delta.g = SoftCompress(delta.g, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        delta.b = SoftCompress(delta.b, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
    }

    // Response curve. Sign-preserving: a brightening and a darkening of the
    // same magnitude get shaped the same way, and 0 stays 0.
    if (kEditGamma != 1.0 && kEditGamma > 0.0) {
        delta = sign(delta) * pow(abs(delta), kEditGamma);
    }

    // Chroma strength. 1 passes the model's per-channel delta; 0 collapses
    // the delta to its luma, so only brightness moves and hue is left alone.
    if (kResidualChroma < 1.0) {
        const float3 kL = float3(0.2126, 0.7152, 0.0722);
        float d_luma = dot(delta, kL);
        delta = lerp(float3(d_luma, d_luma, d_luma), delta, kResidualChroma);
    }

    // Region protection. Both are off at 0, so the default leaves the edit
    // whole across the range.
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


float SoftClampRatio(float r, float lo, float hi) {
    if (r >= 1.0) {
        float span = max(hi - 1.0, 1e-6);
        return 1.0 + (hi - 1.0) * (r - 1.0) / (r - 1.0 + span);
    } else {
        float span = max(1.0 - lo, 1e-6);
        return 1.0 - (1.0 - lo) * (1.0 - r) / ((1.0 - r) + span);
    }
}

// The residual is dimensionless: measure the model's change in its linear
// SDR domain, undo only the deterministic input grade, then apply that gain
// directly to the original HDR linear pixel. Do not round-trip the HDR pixel
// through the SDR tone mapper; doing that makes the same model edit depend on
// the HDR brightness and can damage headroom around highlights.
float3 ComposeRatioDelta(float3 low_in_lin, float3 low_out_lin) {
    const float eps = max(kRatioEps, 1e-8);
    float3 ratio = (max(low_out_lin, 0.0) + eps) /
                   (max(low_in_lin, 0.0) + eps);
    ratio.r = SoftClampRatio(ratio.r, kRatioDarken, kRatioBrighten);
    ratio.g = SoftClampRatio(ratio.g, kRatioDarken, kRatioBrighten);
    ratio.b = SoftClampRatio(ratio.b, kRatioDarken, kRatioBrighten);
    return ratio - 1.0;
}

#endif

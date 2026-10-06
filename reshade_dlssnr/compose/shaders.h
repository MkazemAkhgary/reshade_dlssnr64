// Compose shaders. Blend the model's edit back onto the original source, in
// SDR (kComposeModelOutput) or HDR (kComposeModelOutputHdr). Both carry the
// same residual-edit shaping and luma/chroma blend.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kComposeModelOutputHdrSource = R"(
// The constant buffer is declared in kGradePrelude. The HDR path uses the
// packed gain-map texture instead of the original HDR resource: RGB is the
// full-resolution linear SDR base (1.0 = 100 nits) and A is the raw f32
// log2 luma gain.

#define kLumaStrength    gParams[0].x
#define kChromaStrength  gParams[0].y
#define kSoftCompression gParams[0].z
#define kSoftMaxBrighten gParams[0].w
#define kSoftMaxDarken   gParams[1].x
#define kSoftMaxChroma   gParams[1].y
#define kSoftKnee        gParams[1].z

#define kGradeEnabled    gParams[2].x
#define kGradeLuma       gParams[2].y
#define kGradeChroma     gParams[2].z
#define kGradeLumaMode   gParams[2].w
#define kGradeChromaMode gParams[3].x
#define kGradeBlackLevel gParams[3].y
#define kGradeWhiteLevel gParams[3].z
#define kGradeContrast   gParams[3].w
#define kGradeShadows    gParams[4].x
#define kGradeHighlights gParams[4].y

#define kEditStrength     gParams[6].x
#define kResidualChroma   gParams[6].y
#define kEditGamma        gParams[6].z
#define kHighlightProtect gParams[6].w
#define kShadowProtect    gParams[7].x
#define kRatioEps         gParams[7].y
#define kRatioBrighten    gParams[7].z
#define kRatioDarken      gParams[7].w

// HDR/gain-map region [32,44).
#define kFormat              gParams[8].x
#define kExposure            gParams[8].y
#define kBt2446K1            gParams[8].z
#define kBt2446K2            gParams[8].w
#define kBt2446K4            gParams[9].x
#define kCrosstalk           gParams[9].y
#define kBaseOffset          gParams[9].z
#define kAlternateOffset     gParams[9].w
#define kGainMinLog2         gParams[10].x
#define kGainMaxLog2         gParams[10].y
#define kEffectiveSceneWhite gParams[10].z

// EASU [44,56), RCAS [56,60).
#define kEasuCon0        gParams[11]
#define kEasuCon1        gParams[12]
#define kEasuCon2        gParams[13]
#define kRcasEnabled     gParams[14].x
#define kRcasStrength    gParams[14].y

Texture2D<float4> gOriginal    : register(t0);
Texture2D<float4> gLowInput    : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput    : register(u0);
SamplerState s0 : register(s0);

float SoftCompress(float x, float max_pos, float max_neg, float knee)
{
    knee = max(knee, 1e-6);
    max_pos = max(max_pos, 1e-6);
    max_neg = max(max_neg, 1e-6);
    if (x >= 0.0)
        return max_pos * x / (max_pos + knee * abs(x));
    else
        return max_neg * x / (max_neg + knee * abs(x));
}

float safe_log(float x)
{
    return log(max(x, 0.0) + kRatioEps);
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + kRatioEps);
}

// Blend original and edited with independent luma and chroma strengths.
//
// Chroma is interpolated in *normalized* space (chroma / luma) rather than
// as absolute color-difference values. Absolute Cb/Cr do not scale with
// luminance, so blending them independently of luma causes saturation
// swings: keeping the original's Cb/Cr while adopting the model's luma
// produces a much more saturated color at the new luminance, and the
// reverse produces a pale one. Normalized chroma is scale-invariant and
// preserves perceived hue and saturation.
float3 HdrLumaWeights()
{
    int fmt = (int)(kFormat + 0.5);
    return fmt == 2 ? float3(0.2627, 0.6780, 0.0593)
                    : float3(0.2126, 0.7152, 0.0722);
}

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


// ---------------------------------------------------------------------------
// FSR 1.0 EASU + RCAS
// Copyright (c) 2021 Advanced Micro Devices, Inc. MIT license.
// Ported from ffx_fsr1.h. The gather source is selected by `channel`
// (0 = model input, 1 = model output) so one implementation serves both.
// ---------------------------------------------------------------------------

static const float FSR_RCAS_LIMIT = 0.25 - (1.0 / 16.0);

float4 FsrGatherR(float2 p, int ch) {
    return ch == 0 ? gLowInput.GatherRed(s0, p)
                   : gLowEnhanced.GatherRed(s0, p);
}
float4 FsrGatherG(float2 p, int ch) {
    return ch == 0 ? gLowInput.GatherGreen(s0, p)
                   : gLowEnhanced.GatherGreen(s0, p);
}
float4 FsrGatherB(float2 p, int ch) {
    return ch == 0 ? gLowInput.GatherBlue(s0, p)
                   : gLowEnhanced.GatherBlue(s0, p);
}

void FsrEasuTapF(inout float3 aC, inout float aW, float2 off, float2 dir,
                 float2 len, float lob, float clp, float3 c) {
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * -dir.y) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = 2.0 / 5.0 * d2 - 1;
    float wA = lob * d2 - 1;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

void FsrEasuSetF(inout float2 dir, inout float len, float2 pp,
                 bool biS, bool biT, bool biU, bool biV,
                 float lA, float lB, float lC, float lD, float lE) {
    float w = 0;
    if (biS) w = (1 - pp.x) * (1 - pp.y);
    if (biT) w = pp.x * (1 - pp.y);
    if (biU) w = (1.0 - pp.x) * pp.y;
    if (biV) w = pp.x * pp.y;
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(max(abs(dc), abs(cb)), 1e-6);
    lenX = rcp(lenX);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(max(abs(ec), abs(ca)), 1e-6);
    lenY = rcp(lenY);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

float3 FsrEasuF(uint2 ip, float4 con0, float4 con1, float4 con2, float2 con3,
                int channel) {
    float2 pp = float2(ip) * con0.xy + con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    float2 p0 = fp * con1.xy + con1.zw;
    float2 p1 = p0 + con2.xy;
    float2 p2 = p0 + con2.zw;
    float2 p3 = p0 + con3;

    float4 bczzR = FsrGatherR(p0, channel);
    float4 bczzG = FsrGatherG(p0, channel);
    float4 bczzB = FsrGatherB(p0, channel);
    float4 ijfeR = FsrGatherR(p1, channel);
    float4 ijfeG = FsrGatherG(p1, channel);
    float4 ijfeB = FsrGatherB(p1, channel);
    float4 klhgR = FsrGatherR(p2, channel);
    float4 klhgG = FsrGatherG(p2, channel);
    float4 klhgB = FsrGatherB(p2, channel);
    float4 zzonR = FsrGatherR(p3, channel);
    float4 zzonG = FsrGatherG(p3, channel);
    float4 zzonB = FsrGatherB(p3, channel);

    float4 bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
    float4 ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
    float4 klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
    float4 zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);

    float bL = bczzL.x;
    float cL = bczzL.y;
    float iL = ijfeL.x;
    float jL = ijfeL.y;
    float fL = ijfeL.z;
    float eL = ijfeL.w;
    float kL = klhgL.x;
    float lL = klhgL.y;
    float hL = klhgL.z;
    float gL = klhgL.w;
    float oL = zzonL.z;
    float nL = zzonL.w;

    float2 dir = 0;
    float len = 0;
    FsrEasuSetF(dir, len, pp, true, false, false, false, bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len, pp, false, true, false, false, cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, pp, false, false, true, false, fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len, pp, false, false, false, true, gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(dirR);
    dirR = zro ? 1 : dirR;
    dir.x = zro ? 1 : dir.x;
    dir *= dirR;

    len = len * 0.5;
    len *= len;

    float stretch = (dir.x * dir.x + dir.y * dir.y) *
                    rcp(max(abs(dir.x), abs(dir.y)));
    float2 len2 = float2(1 + (stretch - 1) * len, 1 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = rcp(lob);

    float3 min4 = min(min(min(float3(ijfeR.z, ijfeG.z, ijfeB.z),
                              float3(klhgR.w, klhgG.w, klhgB.w)),
                          float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));
    float3 max4 = max(max(max(float3(ijfeR.z, ijfeG.z, ijfeB.z),
                              float3(klhgR.w, klhgG.w, klhgB.w)),
                          float3(ijfeR.y, ijfeG.y, ijfeB.y)),
                      float3(klhgR.x, klhgG.x, klhgB.x));

    float3 aC = 0;
    float aW = 0;
    FsrEasuTapF(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.x, bczzG.x, bczzB.x));
    FsrEasuTapF(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, float3(bczzR.y, bczzG.y, bczzB.y));
    FsrEasuTapF(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.x, ijfeG.x, ijfeB.x));
    FsrEasuTapF(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, float3(ijfeR.y, ijfeG.y, ijfeB.y));
    FsrEasuTapF(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.z, ijfeG.z, ijfeB.z));
    FsrEasuTapF(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, float3(ijfeR.w, ijfeG.w, ijfeB.w));
    FsrEasuTapF(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.x, klhgG.x, klhgB.x));
    FsrEasuTapF(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, float3(klhgR.y, klhgG.y, klhgB.y));
    FsrEasuTapF(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.z, klhgG.z, klhgB.z));
    FsrEasuTapF(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, float3(klhgR.w, klhgG.w, klhgB.w));
    FsrEasuTapF(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.z, zzonG.z, zzonB.z));
    FsrEasuTapF(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, float3(zzonR.w, zzonG.w, zzonB.w));

    return min(max4, max(min4, aC * rcp(aW)));
}

float3 FsrRcasF(float3 b, float3 d, float3 e, float3 f, float3 h,
                float sharpness) {
    float bR = b.r; float bG = b.g; float bB = b.b;
    float dR = d.r; float dG = d.g; float dB = d.b;
    float eR = e.r; float eG = e.g; float eB = e.b;
    float fR = f.r; float fG = f.g; float fB = f.b;
    float hR = h.r; float hG = h.g; float hB = h.b;

    float bL = bB * 0.5 + (bR * 0.5 + bG);
    float dL = dB * 0.5 + (dR * 0.5 + dG);
    float eL = eB * 0.5 + (eR * 0.5 + eG);
    float fL = fB * 0.5 + (fR * 0.5 + fG);
    float hL = hB * 0.5 + (hR * 0.5 + hG);

    float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
    // HLSL min/max are binary, so the neighborhood extremes are chained
    // rather than written as the three-argument forms the FSR source uses.
    float nz_min = min(min(bL, dL), min(eL, min(fL, hL)));
    float nz_max = max(max(bL, dL), max(eL, max(fL, hL)));
    float nz_range = max(nz_max - nz_min, 1e-6);
    nz = saturate(abs(nz) / nz_range);
    nz = -0.5 * nz + 1.0;

    float mn4R = min(min(bR, dR), min(fR, hR));
    float mn4G = min(min(bG, dG), min(fG, hG));
    float mn4B = min(min(bB, dB), min(fB, hB));
    float mx4R = max(max(bR, dR), max(fR, hR));
    float mx4G = max(max(bG, dG), max(fG, hG));
    float mx4B = max(max(bB, dB), max(fB, hB));

    float hitMinR = min(mn4R, eR) /
                    max(4.0 * mx4R, 1e-6);
    float hitMinG = min(mn4G, eG) /
                    max(4.0 * mx4G, 1e-6);
    float hitMinB = min(mn4B, eB) /
                    max(4.0 * mx4B, 1e-6);

    float hitMaxR = (1.0 - max(mx4R, eR)) /
                    min(4.0 * mn4R - 4.0, -1e-6);
    float hitMaxG = (1.0 - max(mx4G, eG)) /
                    min(4.0 * mn4G - 4.0, -1e-6);
    float hitMaxB = (1.0 - max(mx4B, eB)) /
                    min(4.0 * mn4B - 4.0, -1e-6);

    float lobeR = max(-hitMinR, hitMaxR);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobeB = max(-hitMinB, hitMaxB);
    float lobe = max(-FSR_RCAS_LIMIT,
                     min(max(lobeR, max(lobeG, lobeB)), 0)) * sharpness;
    lobe *= nz;

    float rcpL = rcp(4.0 * lobe + 1.0);
    float3 c;
    c.r = (lobe * bR + lobe * dR + lobe * hR + lobe * fR + eR) * rcpL;
    c.g = (lobe * bG + lobe * dG + lobe * hG + lobe * fG + eG) * rcpL;
    c.b = (lobe * bB + lobe * dB + lobe * hB + lobe * fB + eB) * rcpL;
    return c;
}


// The model's edit as a per-channel delta ratio in linear SDR, measured
// between the two working-scale images the model saw. Delta alone: the caller
// decides which domain to apply it in.
// The residual shaping controls, shared by both compose passes. `luma` is the
// SDR-domain luma of the pixel the edit will land on, used only by the region
// protection; the other three are pixel-independent.
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

// The ratio alone, without any shaping. Kept separate because the shaping
// needs the target pixel's luma, which this does not.
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



float3 SrgbToLinearExtLocal(float3 v)
{
    float3 av = abs(v);
    float3 lo = av / 12.92;
    float3 hi = pow((av + 0.055) / 1.055, 2.4);
    float3 lin = lerp(lo, hi, step(0.04045, av));
    return sign(v) * lin;
}

float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}

float3 ReconstructHdr(float3 sdr_base, float gain)
{
    // A is a single luma gain. Apply the ISO offsets to the luma, recover the
    // original HDR luma, then rescale the SDR RGB triple so chromaticity is
    // preserved. Applying the scalar gain independently to RGB with the
    // offsets would add the same offset to every channel and desaturate dark
    // colours.
    const float3 luma = HdrLumaWeights();
    float y_sdr = max(dot(max(sdr_base, 0.0), luma), 0.0);
    float y_hdr = (y_sdr + kBaseOffset) * exp2(gain) - kAlternateOffset;
    y_hdr = max(y_hdr, 0.0);
    float scale = y_sdr > 1e-8 ? y_hdr / y_sdr : 0.0;
    return max(sdr_base, 0.0) * scale;
}

float3 NitsToPq(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 Y = max(nits / 10000.0, 0.0);
    float3 Ym1 = pow(Y, m1);
    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;
    return pow(num / max(den, 1e-6), m2);
}

float3 EncodeHdr(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        // Gain-map HDR is normalized to 100 nits; scRGB uses 80 nits/unit.
        return lin * (100.0 / 80.0);
    }
    if (fmt == 2) {
        return NitsToPq(max(lin, 0.0) * 100.0);
    }
    return max(lin, 0.0);
}

// `kExposure` is retained in the uniform contract because the exposure EMA
// feeds the effective scene white on the CPU. The split and compose never
// multiply the image by it a second time.
float3 ComposeHdr(float3 sdr_base, float gain,
                  float3 low_in, float3 low_out)
{
    // The model interface is sRGB-encoded SDR. Decode after the model's
    // working-scale resample/sharpening, then undo the optional input grade so
    // the residual is measured in the same linear SDR domain as the gain map.
    float3 low_in_lin = max(SrgbToLinearExtLocal(low_in), 0.0);
    float3 low_out_lin = max(SrgbToLinearExtLocal(low_out), 0.0);
    if (kGradeEnabled > 0.5) {
        low_in_lin = max(GradeLinearInv(low_in_lin), 0.0);
    }

    const float3 kL = HdrLumaWeights();
    const float base_luma = dot(max(sdr_base, 0.0), kL);
    const float3 delta = ShapeResidual(
        ComposeRatioDelta(low_in_lin, low_out_lin), base_luma);

    // The model edits the linear SDR base. Apply that edit in SDR first, then
    // feed both the unedited and edited SDR pixels through the exact same gain
    // map. For the ISO 21496-1 reconstruction
    //
    //   HDR = (SDR + base_offset) * 2^gain - alternate_offset
    //
    // multiplying the reconstructed HDR by an SDR ratio is only equivalent
    // when both offsets are zero. With the chosen 1/64 offsets, the old
    // ratio-lift error grows with 2^gain, which is why high-exposure scenes
    // acquired an exposure-dependent color cast.
    const float3 sdr_edited =
        max(sdr_base * (1.0 + delta), 0.0);

    const float3 hdr_src = ReconstructHdr(sdr_base, gain);
    const float3 hdr_edited = ReconstructHdr(sdr_edited, gain);

    // Blend in HDR linear light only after both variants have passed through
    // the same gain-map reconstruction.
    return BlendYCbCr(hdr_src, hdr_edited,
                      kLumaStrength, kChromaStrength);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 packed = gOriginal.SampleLevel(s0, uv, 0);

    // EASU upsample from the actual model working resolution back to full HDR
    // resolution. Both textures are in the same sRGB-encoded model domain.
    const float2 easu_con3 = float2(0.0, 4.0 * kEasuCon1.y);
    float3 low_in = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                             easu_con3, 0);
    float3 low_out = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                              easu_con3, 1);

    if (kRcasEnabled > 0.5) {
        const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);

        float3 b_in = gLowInput.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_in = gLowInput.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_in = gLowInput.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_in = gLowInput.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_in = FsrRcasF(b_in, d_in, low_in, f_in, h_in, kRcasStrength);

        float3 b_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_out = gLowEnhanced.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_out = gLowEnhanced.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_out = FsrRcasF(b_out, d_out, low_out, f_out, h_out, kRcasStrength);
    }

    const float3 sdr_base = DecodePackedHdr(packed);
    const float3 final_lin = ComposeHdr(sdr_base, packed.a, low_in, low_out);
    const float3 final_out = EncodeHdr(final_lin);

    // Packed resources are linear gain-map data in the 100-nit normalized domain;
    // the destination is the game's original HDR encoding. Alpha is preserved
    // from the packed path only when
    // the target format conceptually carries it; for the game resource the
    // color channels are the only meaningful data.
    gOutput[id.xy] = float4(final_out, 1.0);
}
)";

inline constexpr const char* kComposeModelOutputSource = R"(
// The constant buffer is declared in kGradePrelude, which is prepended to this
// source. Only the pass-specific macros belong here.

#define kLumaStrength     gParams[0].x
#define kChromaStrength   gParams[0].y
#define kSoftCompression  gParams[0].z
#define kSoftMaxBrighten  gParams[0].w
#define kSoftMaxDarken    gParams[1].x
#define kSoftMaxChroma    gParams[1].y
#define kSoftKnee         gParams[1].z
// Residual block, at kResidualSlotBase = float 24 = gParams[6]. See the region
// map in compute_engine.h.
#define kEditStrength     gParams[6].x
#define kResidualChroma   gParams[6].y
#define kEditGamma        gParams[6].z
#define kHighlightProtect gParams[6].w
#define kShadowProtect    gParams[7].x
#define kRatioEps         gParams[7].y
#define kRatioBrighten    gParams[7].z
#define kRatioDarken      gParams[7].w

Texture2D<float4> gOriginal : register(t0);
Texture2D<float4> gLowInput : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float SoftCompress(float x, float max_pos, float max_neg, float knee)
{
    knee = max(knee, 1e-6);
    max_pos = max(max_pos, 1e-6);
    max_neg = max(max_neg, 1e-6);
    if (x >= 0.0)
        return max_pos * x / (max_pos + knee * abs(x));
    else
        return max_neg * x / (max_neg + knee * abs(x));
}

float safe_log(float x)
{
    return log(max(x, 0.0) + max(kRatioEps, 1e-8));
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + max(kRatioEps, 1e-8));
}

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

// Blend original and edited with independent luma and chroma strengths.
// See the comment on the HDR version for why chroma is normalized.
float3 BlendYCbCr(float3 original, float3 edited,
                  float luma_strength, float chroma_strength)
{
    const float3 kL = float3(0.2126, 0.7152, 0.0722);
    const float wr = kL.r;
    const float wg = max(kL.g, 1e-6);
    const float wb = kL.b;
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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 original = gOriginal.SampleLevel(s0, uv, 0);
    float4 low_input = gLowInput.SampleLevel(s0, uv, 0);
    float4 low_enhanced = gLowEnhanced.SampleLevel(s0, uv, 0);

    float3 in_rgb = low_input.rgb;
    float3 out_rgb = low_enhanced.rgb;

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
)";

}  // namespace reshade_dlssnr

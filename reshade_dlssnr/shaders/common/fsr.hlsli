#pragma once
#ifndef DLSSNR_FSR_HLSLI
#define DLSSNR_FSR_HLSLI

// Shared FSR 1.0 EASU + RCAS upsampler. Both SDR and HDR compose passes use
// this exact implementation so reconstruction does not diverge by output
// domain. The source textures are the model input/output at working scale.


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


#endif

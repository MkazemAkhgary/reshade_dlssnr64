#include "compute_passes.h"

#include <cstring>

namespace reshade_dlssnr {

namespace {

constexpr const char* kZeroHistogram = R"(
RWBuffer<uint> gHistogram : register(u0);

[numthreads(256, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < 256u)
        gHistogram[id.x] = 0u;
}
)";

constexpr const char* kBuildHistogram = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = encoding format (0 = SDR sRGB, 1 = scRGB linear, 2 = HDR10 PQ)
// gParams[0].y = luma min log2 (histogram range low edge)
// gParams[0].z = luma max log2 (histogram range high edge)
#define kFormat       gParams[0].x
#define kLumaMinLog2  gParams[0].y
#define kLumaMaxLog2  gParams[0].z

Texture2D<float4> gSource : register(t0);
RWBuffer<uint>    gHistogram : register(u0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

// ST.2084 (PQ) EOTF. Returns nits.
float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
}

float LinearLuma(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        // scRGB: already linear
        return dot(max(c, 0.0), kLuma709);
    } else if (fmt == 2) {
        // HDR10 PQ: decode to nits, normalize to scRGB units (1.0 == 80 nits)
        float3 nits = PqToNits(c);
        return dot(max(nits, 0.0), kLuma709) / 80.0;
    } else {
        // SDR: sRGB-encoded
        return dot(SrgbToLinear(c), kLuma709);
    }
}

// Shared-memory histogram, one per workgroup. Declared at global scope:
// HLSL requires groupshared storage to be declared outside a function.
groupshared uint gSharedHist[256];

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID,
            uint3 gtid : SV_GroupThreadID,
            uint3 dtid : SV_DispatchThreadID)
{
    const uint lidx = gtid.x + gtid.y * 8u;
    for (uint i = lidx; i < 256u; i += 64u)
        gSharedHist[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (dtid.x < gWidth && dtid.y < gHeight) {
        float3 c = gSource.Load(int3(dtid.xy, 0)).rgb;
        float luma = LinearLuma(c);

        // Bin by log2 luminance.
        float l2 = log2(max(luma, 1e-8));
        float span = max(kLumaMaxLog2 - kLumaMinLog2, 1e-4);
        float t = saturate((l2 - kLumaMinLog2) / span);
        uint bin = min((uint)(t * 255.0 + 0.5), 255u);

        InterlockedAdd(gSharedHist[bin], 1u);
    }

    GroupMemoryBarrierWithGroupSync();

    for (uint i = lidx; i < 256u; i += 64u) {
        if (gSharedHist[i] > 0u)
            InterlockedAdd(gHistogram[i], gSharedHist[i]);
    }
}
)";

constexpr const char* kDebugDisplay = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = display mode (0 color, 1 motion, 2 depth)
// gParams[0].y = source content width
// gParams[0].z = source content height
// gParams[0].w = source resource width
// gParams[1].x = source resource height
#define kMode          gParams[0].x
#define kContentW      gParams[0].y
#define kContentH      gParams[0].z
#define kResourceW     gParams[0].w
#define kResourceH     gParams[1].x

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;

    // Map destination UV [0,1] to the source's content region. When the
    // model ran below the frame's resolution its output occupies only the
    // top-left corner of the resource, so we scale the sample accordingly.
    float2 dstUv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float2 srcUv = dstUv * float2(kContentW / kResourceW,
                                  kContentH / kResourceH);

    float4 c = gSource.SampleLevel(s0, srcUv, 0);

    int mode = (int)(kMode + 0.5);
    float3 rgb;
    if (mode == 1) {
        rgb = float3(0.5 + 0.5 * c.x, 0.5 + 0.5 * c.y, 0.0);
    } else if (mode == 2) {
        rgb = c.rrr;
    } else {
        rgb = c.rgb;
    }

    gOutput[id.xy] = float4(rgb, 1.0);
}
)";

constexpr const char* kTonemapForward = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = exposure
// gParams[0].y = white point (Extended Reinhard W, in exposure-adjusted space)
// gParams[0].z = format (0 SDR sRGB, 1 scRGB linear, 2 HDR10 PQ)
#define kExposure   gParams[0].x
#define kWhitePoint gParams[0].y
#define kFormat     gParams[0].z

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}

// ST.2084 (PQ) EOTF. Returns nits.
float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
}

// Decode to a common linear space where 1.0 == scRGB white (80 nits).
float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        // scRGB: already linear
        return max(c, 0.0);
    } else if (fmt == 2) {
        // HDR10 PQ: decode to nits, then to scRGB units
        return max(PqToNits(c), 0.0) / 80.0;
    } else {
        // SDR sRGB
        return SrgbToLinear(c);
    }
}

// Extended Reinhard, applied componentwise to a color. Monotonic in each
// channel, so details survive. With W = 1 this is classic x / (1 + x).
//
// The signature must be float3 -> float3: HLSL silently truncates a float3
// argument to a scalar by taking .x, and then broadcasts the scalar result
// back to float3, which produces greyscale output.
float3 ReinhardForward(float3 x, float W)
{
    float W2 = max(W * W, 1e-6);
    return x * (1.0 + x / W2) / (1.0 + x);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    // The tonemap doubles as the resize: sampling at target UVs with a
    // bilinear filter gives us the working-scale image directly.
    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 src = gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeToLinear(src.rgb);
    lin *= kExposure;

    float3 sdr = ReinhardForward(lin, kWhitePoint);
    float3 out_rgb = LinearToSrgb(sdr);

    gOutput[id.xy] = float4(out_rgb, src.a);
}
)";

constexpr const char* kPrepareModelInput = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
};

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    gOutput[id.xy] = color;
}
)";

constexpr const char* kComposeModelOutputHdr = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// User shader params (identical to ComposeModelOutput):
#define kLumaStrength    gParams[0].x
#define kChromaStrength  gParams[0].y
#define kSoftCompression gParams[0].z
#define kSoftMaxBrighten gParams[0].w
#define kSoftMaxDarken   gParams[1].x
#define kSoftMaxChroma   gParams[1].y
#define kSoftKnee        gParams[1].z
// HDR uniforms, at kHdrSlotBase = float 32 = gParams[8]. See the region map
// in compute_engine.h. These used to sit at gParams[2], directly above the
// parameter table, which was free to grow into them.
//
// Only the format is left. The residual is a ratio, so the tonemap's exposure
// and white point cancel out of the compose entirely and are not read here.
#define kFormat          gParams[8].x
// Residual block, at kResidualSlotBase = float 24 = gParams[6].
#define kEditStrength    gParams[6].x

Texture2D<float4> gOriginal    : register(t0);
Texture2D<float4> gLowInput    : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput    : register(u0);
SamplerState s0 : register(s0);

// --- Decode / encode, matching TonemapForward ---

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}

float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
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
    return pow(num / den, m2);
}

float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        return max(c, 0.0);
    } else if (fmt == 2) {
        return max(PqToNits(c), 0.0) / 80.0;
    } else {
        return SrgbToLinear(c);
    }
}

float3 EncodeFromLinear(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        return max(lin, 0.0);
    } else if (fmt == 2) {
        return NitsToPq(max(lin, 0.0) * 80.0);
    } else {
        return LinearToSrgb(lin);
    }
}

// --- Soft compression, identical to ComposeModelOutput ---

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
    return log(max(x, 0.0) + 1e-6);
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + 1e-6);
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
float3 BlendYCbCr(float3 original, float3 edited,
                  float luma_strength, float chroma_strength)
{
    const float3 kL = float3(0.2126, 0.7152, 0.0722);

    float orig_y = dot(original, kL);
    float edit_y = dot(edited, kL);
    float final_y = lerp(orig_y, edit_y, luma_strength);

    float safe_orig_y = max(orig_y, 1e-4);
    float safe_edit_y = max(edit_y, 1e-4);
    float safe_final_y = max(final_y, 1e-4);

    float orig_cbn = (original.b - orig_y) / (1.8556 * safe_orig_y);
    float orig_crn = (original.r - orig_y) / (1.5748 * safe_orig_y);
    float edit_cbn = (edited.b - edit_y) / (1.8556 * safe_edit_y);
    float edit_crn = (edited.r - edit_y) / (1.5748 * safe_edit_y);

    float final_cbn = lerp(orig_cbn, edit_cbn, chroma_strength);
    float final_crn = lerp(orig_crn, edit_crn, chroma_strength);

    float final_cb = final_cbn * safe_final_y;
    float final_cr = final_crn * safe_final_y;

    float3 result;
    result.r = final_y + 1.5748 * final_cr;
    result.g = final_y - 0.1873 * final_cb - 0.4681 * final_cr;
    result.b = final_y + 1.8556 * final_cb;
    return result;
}

// The model's edit is a per-channel multiplicative ratio in linear SDR,
// applied directly to the HDR linear. A ratio is scale-invariant, so it means
// the same thing here as it does in the SDR compose.
//
// It preserves relative brightness changes exactly, but can amplify noise
// where the SDR input is near zero: the ratio has a small denominator there.
float3 ComposeRatio(float3 src_lin, float3 low_in, float3 low_out) {
    float3 sdr_in_lin = SrgbToLinear(low_in);
    float3 sdr_out_lin = SrgbToLinear(low_out);

    const float eps = 1e-4;
    float3 ratio = (sdr_out_lin + eps) / (sdr_in_lin + eps);
    // Same guard as the SDR compose: where the model's input was near black
    // this ratio has a small denominator and can otherwise reach thousands,
    // blowing those pixels out. Bound it to 16x either way.
    ratio = clamp(ratio, 1.0 / 16.0, 16.0);
    float3 ratio_delta = ratio - 1.0;

    if (kSoftCompression != 0) {
        ratio_delta.r = SoftCompress(
            ratio_delta.r, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        ratio_delta.g = SoftCompress(
            ratio_delta.g, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        ratio_delta.b = SoftCompress(
            ratio_delta.b, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
    }

    ratio_delta *= kEditStrength;
    return max(src_lin, 0.0) * (1.0 + ratio_delta);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 orig_hdr = gOriginal.SampleLevel(s0, uv, 0);
    float3 low_in  = gLowInput.SampleLevel(s0, uv, 0).rgb;    // sRGB
    float3 low_out = gLowEnhanced.SampleLevel(s0, uv, 0).rgb; // sRGB

    float3 src_lin = DecodeToLinear(orig_hdr.rgb);

    float3 edited_src_lin = ComposeRatio(src_lin, low_in, low_out);

    // Blend original and edited in linear space using luma / chroma
    // strengths. Chroma is interpolated in normalized space so that
    // moving luma alone preserves perceived saturation, and moving
    // chroma alone preserves brightness.
    float3 final_lin = BlendYCbCr(src_lin, edited_src_lin,
                                  kLumaStrength, kChromaStrength);

    // Encode back to the source's own format.
    float3 final_out = EncodeFromLinear(max(final_lin, 0.0));

    gOutput[id.xy] = float4(final_out, orig_hdr.a);
}
)";

constexpr const char* kComposeModelOutput = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

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
    return log(max(x, 0.0) + 1e-6);
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + 1e-6);
}

// Blend original and edited with independent luma and chroma strengths.
// See the comment on the HDR version for why chroma is normalized.
float3 BlendYCbCr(float3 original, float3 edited,
                  float luma_strength, float chroma_strength)
{
    const float3 kL = float3(0.2126, 0.7152, 0.0722);

    float orig_y = dot(original, kL);
    float edit_y = dot(edited, kL);
    float final_y = lerp(orig_y, edit_y, luma_strength);

    float safe_orig_y = max(orig_y, 1e-4);
    float safe_edit_y = max(edit_y, 1e-4);
    float safe_final_y = max(final_y, 1e-4);

    float orig_cbn = (original.b - orig_y) / (1.8556 * safe_orig_y);
    float orig_crn = (original.r - orig_y) / (1.5748 * safe_orig_y);
    float edit_cbn = (edited.b - edit_y) / (1.8556 * safe_edit_y);
    float edit_crn = (edited.r - edit_y) / (1.5748 * safe_edit_y);

    float final_cbn = lerp(orig_cbn, edit_cbn, chroma_strength);
    float final_crn = lerp(orig_crn, edit_crn, chroma_strength);

    float final_cb = final_cbn * safe_final_y;
    float final_cr = final_crn * safe_final_y;

    float3 result;
    result.r = final_y + 1.5748 * final_cr;
    result.g = final_y - 0.1873 * final_cb - 0.4681 * final_cr;
    result.b = final_y + 1.8556 * final_cb;
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

    // A ratio has a small denominator wherever the model's input was near
    // black, so an unguarded log residual can reach hundreds and blow those
    // pixels out. Bound it to a change no real edit should need — 16x either
    // way — which only ever bites in exactly that near-black case. The
    // compression sliders bound it further when they are on.
    const float kMaxLogRatio = 2.7725887;  // log(16)
    residual_log = clamp(residual_log, -kMaxLogRatio, kMaxLogRatio);

    // A ratio is scale-invariant, so the same edit means the same thing
    // whatever range the original is in. Scaling the log raises the ratio to
    // the strength, which is the ratio-space analogue of scaling a delta: 0
    // leaves the image alone, 1 applies the model's own edit.
    float3 edited = original.rgb * exp(residual_log * kEditStrength);

    float3 final_rgb = BlendYCbCr(original.rgb, edited,
                                  kLumaStrength, kChromaStrength);
    final_rgb = 0.5 * (final_rgb + sqrt(final_rgb * final_rgb + 1e-6));

    gOutput[id.xy] = float4(final_rgb, original.a);
}
)";

}  // namespace

constexpr const char* kApplyGrade = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// Grade block, at kGradeSlotBase = float 8 = gParams[2]. See the region map
// in compute_engine.h.
#define kLuma        gParams[2].x
#define kChroma      gParams[2].y
#define kLumaMode    gParams[2].z
#define kChromaMode  gParams[2].w
#define kBlackPoint  gParams[3].x
#define kWhitePoint  gParams[3].y
#define kContrast    gParams[3].z
#define kShadows     gParams[3].w
#define kHighlights  gParams[4].x
#define kFormat      gParams[4].y

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

// sRGB 0.5 expressed in linear light. Contrast pivots here rather than at 0.5
// linear so that "no change at mid grey" means the mid grey the user sees.
static const float kMidGreyLinear = 0.2140;

// The most a shaping control scales a pixel by at full amount, at the peak of
// its region.
static const float kShapeMax = 0.5;

// --- Decode / encode, matching ComposeModelOutputHdr -----------------------
//
// The grade runs in linear light. Every operator below is a statement about
// light, and applying it to a display-encoded value is what bends a highlight
// the wrong way and loses detail in the shadows.

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

float3 LinearToSrgb(float3 v)
{
    v = saturate(v);
    return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}

float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
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
    return pow(num / den, m2);
}

float3 DecodeToLinear(float3 c)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        return max(c, 0.0);
    } else if (fmt == 2) {
        return max(PqToNits(c), 0.0) / 80.0;
    } else {
        return SrgbToLinear(c);
    }
}

float3 EncodeFromLinear(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 1) {
        return max(lin, 0.0);
    } else if (fmt == 2) {
        return NitsToPq(max(lin, 0.0) * 80.0);
    } else {
        return LinearToSrgb(lin);
    }
}

// SDR is the only target with a hard ceiling: its encode is a saturate to
// [0,1], so anything above white is thrown away and a bright highlight
// flattens to a disc. Bending the response over above the knee keeps a
// highlight's shape as it approaches the ceiling instead.
//
// scRGB and HDR10 have real headroom — 80 nits per unit and 10000 nits — so
// they are left alone and a highlight is allowed to be brighter than white.
float RollOff(float x)
{
    const float knee = 0.75;
    const float room = 1.0 - knee;
    // Single return with an initialised local: see GradeLuma.
    float rolled = x;
    if (x > knee) {
        rolled = knee + room * (1.0 - exp(-(x - knee) / room));
    }
    return rolled;
}

// Per channel, because that is what keeps each channel inside the ceiling: a
// luma-preserving rolloff can leave one channel above it and hand the encode
// a value it will clip anyway.
float3 PrepareForOutput(float3 lin)
{
    int fmt = (int)(kFormat + 0.5);
    if (fmt == 0) {
        lin.r = RollOff(lin.r);
        lin.g = RollOff(lin.g);
        lin.b = RollOff(lin.b);
    }
    return EncodeFromLinear(lin);
}

// --- Levels ----------------------------------------------------------------
//
// Black point sets the floor and white point sets what lands on white, in
// that order, so raising the black point does not also darken white.

float ApplyLevels(float y)
{
    // 0.05 rather than an epsilon: the two controls are independent sliders,
    // and a span near zero would turn a contradictory pair into a huge gain.
    float span = max(kWhitePoint - kBlackPoint, 0.05);
    return max(y - kBlackPoint, 0.0) / span;
}

// Contrast, a stretch around mid grey. Positive steepens and negative
// flattens. Clamped at black because the stretch drives the darkest values
// negative on the way down, and black is already as dark as the range goes.
float ApplyContrast(float y)
{
    // Single return with an initialised local: see GradeLuma.
    float graded = y;
    if (kContrast != 0.0) {
        float slope = exp2(kContrast);
        graded = max(kMidGreyLinear + (y - kMidGreyLinear) * slope, 0.0);
    }
    return graded;
}

// --- Shaping ---------------------------------------------------------------
//
// Two region weights, wide enough to overlap through the midtones so the two
// controls do not fight over the same pixels, and each falling to near zero
// at the opposite end. That is what stops a shadows push from also moving
// white and a highlights push from also moving black.

float ShadowWeight(float y)
{
    float t = saturate(y);
    return exp(-t * t * 6.0);
}

float HighlightWeight(float y)
{
    float t = 1.0 - saturate(y);
    return exp(-t * t * 6.0);
}

// A scale rather than an offset. Linear light is unbounded and scale-free, so
// a factor means the same thing at any exposure; an offset that is right for
// a midtone is a white-out on a deep shadow, which is the failure mode this
// avoids. Values above white scale too, so HDR highlights keep responding.
float Shape(float y, float amount, float weight)
{
    return y * (1.0 + amount * kShapeMax * weight);
}

// --- Luma ------------------------------------------------------------------
//
// Each mode maps a luma to a new luma for an amount in [-1, 1], where 0 means
// unchanged. They differ in where in the range they act and in what they do
// at the ends, which is the whole reason the UI has a combo rather than a
// stack: they are alternatives, not layers.
//
// Full travel is strong but recoverable in every mode. Nothing here may
// collapse the image at the end of its slider, which is what an unscaled lerp
// to white or an unbounded offset does.

// Single return with an initialised local rather than an early return per
// mode: fxc's flow analysis reports X4000 on the multi-return form even though
// every path returns a value, and that same warning code is real elsewhere.
float GradeLuma(float y, float amount, int mode)
{
    float graded = y;  // amount 0, and any mode not named below

    if (amount != 0.0) {
        if (mode == 0) {
            // Gain. Exponential so the control is symmetric: +1 doubles and
            // -1 halves. On linear light this is an exposure, and a value
            // above white scales with everything else rather than clipping.
            graded = y * exp2(amount);
        } else if (mode == 1) {
            // Gamma. Endpoints are fixed, so it neither clips nor lifts black
            // and the effect concentrates in the midtones. The input is not
            // clamped: a value above white is raised to the same power as the
            // rest, so this stays a gamma rather than a gamma plus a clip.
            graded = pow(max(y, 0.0), exp2(-amount));
        } else if (mode == 2) {
            // Lift. Raises the black level: positive moves a pixel up by the
            // room above it and negative down by the room below, so nothing
            // crosses an end. The travel is halved, because at full amount an
            // unscaled lift puts black on white.
            float lift = amount * 0.5;
            float room = lift > 0.0 ? max(1.0 - y, 0.0) : y;
            graded = y + lift * room;
        } else {
            // Soft. A parabolic lift, zero at both ends and strongest at mid
            // grey, so the middle opens up while black and white stay put.
            // This is the one to reach for when a large amount would clip.
            graded = y + amount * y * max(1.0 - y, 0.0);
        }
    }

    return graded;
}

// --- Chroma ----------------------------------------------------------------

float3 GradeChroma(float3 c, float amount, int mode)
{
    float3 graded = c;  // amount 0

    if (amount != 0.0) {
        float y = dot(c, kLuma709);
        float scale = 1.0 + amount;

        if (mode == 0) {
            // Saturation. A straight blend toward luma, so every pixel moves
            // by the same factor. The predictable default.
            graded = lerp(float3(y, y, y), c, scale);
        } else if (mode == 1) {
            // Vibrance. The scale is weighted by how muted the pixel already
            // is: colours near grey move at full strength and vivid ones at
            // half, so a vivid pixel still responds to the control instead of
            // being frozen out of it.
            float spread =
                max(max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)), 0.0);
            graded = lerp(float3(y, y, y), c,
                          1.0 + amount * (1.0 - 0.5 * saturate(spread)));
        } else {
            // ChromaGain. Scale only the chroma channels of a YCbCr
            // decomposition, so luma passes through untouched. Use when
            // saturation must not move brightness at all.
            float cb = (c.b - y) / 1.8556 * scale;
            float cr = (c.r - y) / 1.5748 * scale;
            graded.r = y + 1.5748 * cr;
            graded.g = y - 0.1873 * cb - 0.4681 * cr;
            graded.b = y + 1.8556 * cb;
        }
    }

    return graded;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 src = gSource.SampleLevel(s0, uv, 0);

    // Decode once, from the target's own encoding. Everything below this
    // point is linear light.
    float3 lin = DecodeToLinear(src.rgb);
    float y = dot(lin, kLuma709);

    // Levels, then shaping, then the luma operator. Levels come first because
    // they define the range everything else is expressed in, and the operator
    // comes last because it is the one the mode combo selects.
    float new_y = ApplyLevels(y);
    new_y = ApplyContrast(new_y);
    new_y = Shape(new_y, kShadows, ShadowWeight(new_y));
    new_y = Shape(new_y, kHighlights, HighlightWeight(new_y));
    new_y = GradeLuma(new_y, kLuma, (int)(kLumaMode + 0.5));

    // Scale all three channels to hit the new luma rather than adding to it,
    // which keeps hue where it was. A pixel at black has no ratio to scale, so
    // it takes the value directly — otherwise a lift could never raise it.
    float3 graded = y > 1e-4 ? lin * (new_y / y) : float3(new_y, new_y, new_y);

    graded = GradeChroma(graded, kChroma, (int)(kChromaMode + 0.5));

    gOutput[id.xy] = float4(PrepareForOutput(graded), src.a);
}
)";

const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",         kZeroHistogram},
    {"BuildHistogram",        kBuildHistogram},
    {"TonemapForward",        kTonemapForward},
    {"PrepareModelInput",     kPrepareModelInput},
    {"ComposeModelOutput",    kComposeModelOutput},
    {"ComposeModelOutputHdr", kComposeModelOutputHdr},
    {"DebugDisplay",          kDebugDisplay},
    {"ApplyGrade",            kApplyGrade},
};

const uint32_t kComputePassCount =
    sizeof(kComputePasses) / sizeof(kComputePasses[0]);

const char* FindComputePassSource(const char* name) {
  if (name == nullptr) return nullptr;
  for (uint32_t i = 0; i < kComputePassCount; ++i) {
    if (std::strcmp(kComputePasses[i].name, name) == 0) {
      return kComputePasses[i].hlsl_source;
    }
  }
  return nullptr;
}

}  // namespace reshade_dlssnr

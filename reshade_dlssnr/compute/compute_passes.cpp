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
    float4 gParams[4];
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
    float4 gParams[4];
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
    float4 gParams[4];
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
    float4 gParams[4];
};

// User shader params (identical to ComposeModelOutput):
#define kLumaStrength    gParams[0].x
#define kChromaStrength  gParams[0].y
#define kSoftCompression gParams[0].z
#define kSoftMaxBrighten gParams[0].w
#define kSoftMaxDarken   gParams[1].x
#define kSoftMaxChroma   gParams[1].y
#define kSoftKnee        gParams[1].z
// HDR uniforms:
#define kExposure        gParams[2].x
#define kWhitePoint      gParams[2].y
#define kFormat          gParams[2].z
#define kEditStrength    gParams[2].w

Texture2D<float4> gOriginal    : register(t0);
Texture2D<float4> gLowInput    : register(t1);
Texture2D<float4> gLowEnhanced : register(t2);
RWTexture2D<float4> gOutput    : register(u0);
SamplerState s0 : register(s0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

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

// --- Extended Reinhard, forward and exact inverse ---

float3 ReinhardForward(float3 x, float W)
{
    float W2 = max(W * W, 1e-6);
    return x * (1.0 + x / W2) / (1.0 + x);
}

// Closed-form inverse of ReinhardForward. Solving:
//   s(1+x) = x + x²/W²
//   x²/W² + (1-s)x - s = 0
//   x = [-(1-s)W² + sqrt((1-s)²W⁴ + 4sW²)] / 2
float3 ReinhardInverse(float3 s, float W)
{
    float W2 = max(W * W, 1e-6);
    s = saturate(s);
    float3 om = 1.0 - s;
    float3 disc = om * om * W2 * W2 + 4.0 * s * W2;
    float3 num = -om * W2 + sqrt(max(disc, 0.0));
    return num * 0.5;
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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);

    float4 orig_hdr = gOriginal.SampleLevel(s0, uv, 0);
    float3 low_in  = gLowInput.SampleLevel(s0, uv, 0).rgb;    // sRGB
    float3 low_out = gLowEnhanced.SampleLevel(s0, uv, 0).rgb; // sRGB

    // Model edit, computed exactly as in the SDR path (in sRGB space).
    float3 residual;
    if (kSoftCompression != 0) {
        float in_luma  = max(dot(low_in, kLuma709), 1e-6);
        float out_luma = max(dot(low_out, kLuma709), 1e-6);

        float residual_luma = safe_log(out_luma) - safe_log(in_luma);
        float3 residual_chroma =
            safe_log3(low_out) - safe_log(out_luma) -
            safe_log3(low_in)  + safe_log(in_luma);

        residual_luma = SoftCompress(
            residual_luma, kSoftMaxBrighten, kSoftMaxDarken, kSoftKnee);
        residual_chroma.x = SoftCompress(
            residual_chroma.x, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);
        residual_chroma.y = SoftCompress(
            residual_chroma.y, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);
        residual_chroma.z = SoftCompress(
            residual_chroma.z, kSoftMaxChroma, kSoftMaxChroma, kSoftKnee);

        float edited_luma = in_luma * exp(residual_luma);
        float3 edited_chroma =
            exp(safe_log3(low_in) - safe_log(in_luma) + residual_chroma);
        float3 edited_rgb = edited_luma * edited_chroma;

        residual = edited_rgb - low_in;
    } else {
        residual = low_out - low_in;
    }
    residual *= kEditStrength;

    // Rebuild the full-res SDR the original would have tonemapped to.
    // This is the same pipeline as TonemapForward, applied to the full-res
    // source instead of the working-scale input.
    float3 src_lin = DecodeToLinear(orig_hdr.rgb);
    float3 sdr_orig_lin = ReinhardForward(src_lin * kExposure, kWhitePoint);
    float3 sdr_orig = LinearToSrgb(sdr_orig_lin);

    // Apply the residual in sRGB space, then inverse-tonemap back to HDR.
    float3 edited_sdr = saturate(sdr_orig + residual);
    float3 edited_sdr_lin = SrgbToLinear(edited_sdr);
    float3 edited_src_lin =
        ReinhardInverse(edited_sdr_lin, kWhitePoint) / kExposure;

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
    float4 gParams[4];
};

#define kLumaStrength     gParams[0].x
#define kChromaStrength   gParams[0].y
#define kSoftCompression  gParams[0].z
#define kSoftMaxBrighten  gParams[0].w
#define kSoftMaxDarken    gParams[1].x
#define kSoftMaxChroma    gParams[1].y
#define kSoftKnee         gParams[1].z

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

    float3 residual;
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

        float edited_luma = in_luma * exp(residual_luma);
        float3 edited_chroma =
            exp(safe_log3(in_rgb) - safe_log(in_luma) + residual_chroma);
        float3 edited_rgb = edited_luma * edited_chroma;

        residual = edited_rgb - in_rgb;
    } else {
        residual = out_rgb - in_rgb;
    }

    float3 edited = original.rgb + residual;

    float3 final_rgb = BlendYCbCr(original.rgb, edited,
                                  kLumaStrength, kChromaStrength);
    final_rgb = 0.5 * (final_rgb + sqrt(final_rgb * final_rgb + 1e-6));

    gOutput[id.xy] = float4(final_rgb, original.a);
}
)";

}  // namespace

const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",         kZeroHistogram},
    {"BuildHistogram",        kBuildHistogram},
    {"TonemapForward",        kTonemapForward},
    {"PrepareModelInput",     kPrepareModelInput},
    {"ComposeModelOutput",    kComposeModelOutput},
    {"ComposeModelOutputHdr", kComposeModelOutputHdr},
    {"DebugDisplay",          kDebugDisplay},
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

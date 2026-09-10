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

// Extended Reinhard, per channel. Monotonic, preserves details. With
// W = 1 this is classic x / (1 + x).
float ReinhardForward(float x, float W)
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

    float original_y = dot(original.rgb, float3(0.2126, 0.7152, 0.0722));
    float edited_y   = dot(edited,   float3(0.2126, 0.7152, 0.0722));
    float final_y = lerp(original_y, edited_y, kLumaStrength);

    float original_cb = (original.b - original_y) / 1.8556;
    float original_cr = (original.r - original_y) / 1.5748;
    float edited_cb   = (edited.b   - edited_y)   / 1.8556;
    float edited_cr   = (edited.r   - edited_y)   / 1.5748;

    float final_cb = lerp(original_cb, edited_cb, kChromaStrength);
    float final_cr = lerp(original_cr, edited_cr, kChromaStrength);

    float3 final_rgb;
    final_rgb.r = final_y + 1.5748 * final_cr;
    final_rgb.g = final_y - 0.1873 * final_cb - 0.4681 * final_cr;
    final_rgb.b = final_y + 1.8556 * final_cb;

    final_rgb = 0.5 * (final_rgb + sqrt(final_rgb * final_rgb + 1e-6));

    gOutput[id.xy] = float4(final_rgb, original.a);
}
)";

}  // namespace

const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",      kZeroHistogram},
    {"BuildHistogram",     kBuildHistogram},
    {"TonemapForward",     kTonemapForward},
    {"PrepareModelInput",  kPrepareModelInput},
    {"ComposeModelOutput", kComposeModelOutput},
    {"DebugDisplay",       kDebugDisplay},
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

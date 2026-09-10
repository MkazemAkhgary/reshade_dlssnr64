#include "compute_passes.h"

#include <cstring>

namespace reshade_dlssnr {

namespace {

constexpr const char* kMeasureWhitePoint = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[4];
};

Texture2D<float4> gSource : register(t0);
RWTexture2D<float> gWhitePoint : register(u0);

// 8x8 dispatch: each thread reduces one tile of the frame and writes that
// tile's maximum luma. The consumer takes the max across the 64 tiles.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 8 || id.y >= 8)
        return;

    uint tileW = max((gWidth + 7u) / 8u, 1u);
    uint tileH = max((gHeight + 7u) / 8u, 1u);
    uint x0 = id.x * tileW;
    uint y0 = id.y * tileH;
    uint x1 = min(x0 + tileW, gWidth);
    uint y1 = min(y0 + tileH, gHeight);

    // Cap the sample count per tile so the pass scales to any resolution.
    uint stepX = max(1u, (x1 - x0) / 32u);
    uint stepY = max(1u, (y1 - y0) / 32u);

    float maxLuma = 0.0;
    for (uint y = y0; y < y1; y += stepY) {
        for (uint x = x0; x < x1; x += stepX) {
            float3 c = gSource.Load(int3(x, y, 0)).rgb;
            maxLuma = max(maxLuma, dot(c, float3(0.2126, 0.7152, 0.0722)));
        }
    }

    gWhitePoint[id.xy] = maxLuma;
}
)";

constexpr const char* kPrepareModelInput = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[4];
};

Texture2D<float4> gSource : register(t0);
Texture2D<float>  gWhitePoint : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

// Reduce the 8x8 tile buffer to a single scalar. The max is taken as-is;
// callers wrap with max(., 1.0) so SDR content is left untouched.
float WhitePoint()
{
    uint w = 0, h = 0;
    gWhitePoint.GetDimensions(w, h);
    float W = 0.0;
    for (uint y = 0; y < h; ++y) {
        for (uint x = 0; x < w; ++x) {
            W = max(W, gWhitePoint.Load(int3(x, y, 0)));
        }
    }
    return W;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float W = max(WhitePoint(), 1.0);

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    gOutput[id.xy] = float4(color.rgb / W, color.a);
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
Texture2D<float>  gWhitePoint : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float WhitePoint()
{
    uint w = 0, h = 0;
    gWhitePoint.GetDimensions(w, h);
    float W = 0.0;
    for (uint y = 0; y < h; ++y) {
        for (uint x = 0; x < w; ++x) {
            W = max(W, gWhitePoint.Load(int3(x, y, 0)));
        }
    }
    return W;
}

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

    // The white point maps the HDR source into [0, 1] for the model. It is
    // clamped to >= 1.0 so SDR content is left byte-for-byte unchanged.
    float W = max(WhitePoint(), 1.0);

    float4 originalHdr = gOriginal.SampleLevel(s0, uv, 0);
    float4 original = float4(originalHdr.rgb / W, originalHdr.a);
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

    // Map back to HDR. When the model left the frame untouched this is the
    // exact inverse of the normalization above.
    float3 final_hdr = final_rgb * W;

    gOutput[id.xy] = float4(final_hdr, original.a);
}
)";

}  // namespace

const ComputePassDef kComputePasses[] = {
    {"MeasureWhitePoint",  kMeasureWhitePoint},
    {"PrepareModelInput",  kPrepareModelInput},
    {"ComposeModelOutput", kComposeModelOutput},
    // Add new passes here, e.g.:
    // {"PreExposure",     kPreExposure},
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

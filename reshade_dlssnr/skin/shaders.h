// Skin mask shader. Compares the detection model's input and output,
// thresholds the difference, spatially smooths, and applies a temporal EMA
// against the previous frame's mask.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kSkinMaskDiffSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = absolute luma threshold (noise floor)
// gParams[0].y = relative luma threshold (fraction of luma)
// gParams[0].z = chroma threshold
// gParams[0].w = saturation bound
// gParams[1].x = edit strength (amplifies the model's diff)
// gParams[1].y = mask smoothing (0..1 -> majority radius 0..2)
// gParams[1].z = temporal EMA alpha
#define kLumaAbs       gParams[0].x
#define kLumaRel       gParams[0].y
#define kChromaAbs     gParams[0].z
#define kSatBound      gParams[0].w
#define kEditStrength  gParams[1].x
#define kMaskSmoothing gParams[1].y
#define kMaskAlpha     gParams[1].z

Texture2D<float4> gInput    : register(t0);
Texture2D<float4> gOutput   : register(t1);
Texture2D<float>  gPrevMask : register(t2);
RWTexture2D<float> gMask    : register(u0);

static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

bool PixelIsSkin(int2 xy)
{
    float3 a = gInput.Load(int3(xy, 0)).rgb;
    float3 b = gOutput.Load(int3(xy, 0)).rgb;

    float lo = 1.0 - kSatBound;
    if (max(max(a.r, a.g), a.b) >= kSatBound) return false;
    if (max(max(b.r, b.g), b.b) >= kSatBound) return false;
    if (min(min(a.r, a.g), a.b) <= lo) return false;
    if (min(min(b.r, b.g), b.b) <= lo) return false;

    float a_luma = dot(a, kLuma709);
    float b_luma = dot(b, kLuma709);
    float luma_diff = abs(b_luma - a_luma) * kEditStrength;
    float luma_ref = max(a_luma, b_luma);

    bool luma_edit = luma_diff > kLumaAbs && luma_diff > kLumaRel * luma_ref;

    float3 chroma_diff = (b - b_luma) - (a - a_luma);
    bool chroma_edit = length(chroma_diff) * kEditStrength > kChromaAbs;

    return luma_edit || chroma_edit;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;

    int r = (int)(kMaskSmoothing * 2.0 + 0.5);
    int2 center = int2(id.xy);
    int hits = 0;
    int total = 0;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int2 p = center + int2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= (int)gWidth || p.y >= (int)gHeight)
                continue;
            total++;
            if (PixelIsSkin(p)) hits++;
        }
    }
    float vote = (total > 0 && hits * 2 >= total) ? 1.0 : 0.0;
    float prev = gPrevMask.Load(int3(id.xy, 0));
    gMask[id.xy] = lerp(prev, vote, kMaskAlpha);
}
)";

}  // namespace reshade_dlssnr

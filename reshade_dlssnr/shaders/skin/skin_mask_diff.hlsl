#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"
#include "common/luma.hlsli"

// Skin-mask parameters live in the generic shader block for this pass.
#define kLumaAbs       shader.Params0.x
#define kLumaRel       shader.Params0.y
#define kChromaAbs     shader.Params0.z
#define kSatBound      shader.Params0.w
#define kSkinMaskEditStrength shader.Params1.x
#define kMaskSmoothing shader.Params1.y
#define kMaskAlpha     shader.Params1.z
#define kTargetMode     shader.Params1.w

// t0 is the exact input image submitted to the model; t1 is its model output.
Texture2D<float4> gInput    : register(t0);
Texture2D<float4> gOutput   : register(t1);
Texture2D<float>  gPrevMask : register(t2);
RWTexture2D<float> gMask    : register(u0);

bool PixelIsSkin(int2 xy)
{
    bool result = false;
    float3 a = gInput.Load(int3(xy, 0)).rgb;
    float3 b = gOutput.Load(int3(xy, 0)).rgb;

    float lo = 1.0 - kSatBound;
    bool range_ok =
        max(max(a.r, a.g), a.b) < kSatBound &&
        max(max(b.r, b.g), b.b) < kSatBound &&
        min(min(a.r, a.g), a.b) > lo &&
        min(min(b.r, b.g), b.b) > lo;

    // During a fresh mask generation, do not sample the newly allocated,
    // uninitialized history texture. Alpha 1 means this frame replaces history.
    float prev = kMaskAlpha >= 0.999
        ? 0.0 : gPrevMask.Load(int3(xy, 0));
    float hysteresis = lerp(1.0, 0.5, saturate(prev * 2.0));

    float a_luma = dot(a, kLuma709);
    float b_luma = dot(b, kLuma709);
    float luma_diff = abs(b_luma - a_luma) * kSkinMaskEditStrength;
    float luma_ref = max(a_luma, b_luma);

    bool luma_edit = luma_diff > (kLumaAbs * hysteresis) &&
                     luma_diff > (kLumaRel * luma_ref * hysteresis);

    float3 chroma_diff = (b - b_luma) - (a - a_luma);
    bool chroma_edit =
        length(chroma_diff) * kSkinMaskEditStrength > (kChromaAbs * hysteresis);
    bool model_edit = luma_edit || chroma_edit;

    const int target = (int)(kTargetMode + 0.5);
    result = target == 0 ? (range_ok && model_edit) : model_edit;

    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    int r = (int)(kMaskSmoothing * 2.0 + 0.5);
    int2 center = int2(id.xy);
    int hits = 0;
    int total = 0;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            int2 p = center + int2(dx, dy);
            if (p.x < 0 || p.y < 0 || p.x >= (int)Width || p.y >= (int)Height)
                continue;
            total++;
            if (PixelIsSkin(p)) hits++;
        }
    }
    float vote = (total > 0 && hits * 2 >= total) ? 1.0 : 0.0;
    float prev = kMaskAlpha >= 0.999
        ? 0.0 : gPrevMask.Load(int3(id.xy, 0));
    gMask[id.xy] = lerp(prev, vote, kMaskAlpha);
}

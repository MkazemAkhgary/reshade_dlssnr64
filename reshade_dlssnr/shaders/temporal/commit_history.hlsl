#include "common/bindings.hlsli"

Texture2D<float4> gInput : register(t0);
Texture2D<float4> gOutput : register(t1);
Texture2D<float4> gValidated : register(t2);
Texture2D<float> gCurrentDepth : register(t3);
RWTexture2D<float4> gHistoryEdit : register(u0);
RWTexture2D<float> gHistoryDepth : register(u1);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kLn2 = 0.69314718056;
static const float kSignalFloor = 0.005;

float4 ExtractFreshEdit(uint2 pixel)
{
    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    const float4 input = gInput.Load(int3(pixel, 0));
    const float4 output = gOutput.Load(int3(pixel, 0));
    const float3 a = max(input.rgb, 0.0);
    const float3 b = max(output.rgb, 0.0);
    const float yi = dot(a, kLuma);
    const float yo = dot(b, kLuma);
    if (all(isfinite(input)) && all(isfinite(output)) &&
        yi >= kSignalFloor && yo >= kSignalFloor) {
        const float gy = log(clamp(yo / yi, 0.5, 2.0));
        const float eps = max(1.0e-4, yi * 0.01);
        const float3 raw = log((b + eps) / (a + eps));
        const float3 q = clamp(raw - gy, -kLn2, kLn2);
        const float3 tau = max(float3(kSignalFloor, kSignalFloor, kSignalFloor), yi * 0.02);
        const float3 omega = max(float3(0.01, 0.01, 0.01), yi * 0.10);
        const float3 confidence = saturate((min(a, b) - tau) / omega);
        const float3 gain = clamp(gy + confidence * q, -kLn2, kLn2);
        result = float4(gain, 1.0);
    }
    return result;
}

float ReadCurrentDepth(uint2 pixel)
{
    float result = 0.0;
    uint dw, dh;
    gCurrentDepth.GetDimensions(dw, dh);
    if (dw > 0u && dh > 0u) {
        const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
        const float2 base = kTemporalGuideBase;
        const float2 size = kTemporalGuideSize;
        const float2 source_pixel = base + uv * size;
        if (all(source_pixel >= 0.0) &&
            all(source_pixel < float2(dw, dh))) {
            const uint2 p = min(uint2(source_pixel), uint2(dw - 1u, dh - 1u));
            const float depth = gCurrentDepth.Load(int3(p, 0));
            result = isfinite(depth) ? depth : 0.0;
        }
    }
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const uint2 pixel = id.xy;
    if (kTemporalUseValidatedHistory > 0.5) {
        const float4 candidate = gValidated.Load(int3(pixel, 0));
        const bool can_commit = candidate.a == 1.0 || candidate.a == 2.0;
        gHistoryEdit[pixel] = float4(
            can_commit ? clamp(candidate.rgb, -kLn2, kLn2) : float3(0.0, 0.0, 0.0), 1.0);
    } else {
        gHistoryEdit[pixel] = ExtractFreshEdit(pixel);
    }
    gHistoryDepth[pixel] = ReadCurrentDepth(pixel);
}

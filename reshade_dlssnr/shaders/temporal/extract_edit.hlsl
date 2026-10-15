#include "common/bindings.hlsli"

Texture2D<float4> gInput : register(t0);
Texture2D<float4> gOutput : register(t1);
Texture2D<float4> gUnused2 : register(t2);
Texture2D<float4> gUnused3 : register(t3);
RWTexture2D<float4> gEdit : register(u0);

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kLn2 = 0.69314718056;
static const float kSignalFloor = 0.005;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const int3 p = int3(id.xy, 0);
    const float4 input = gInput.Load(p);
    const float4 output = gOutput.Load(p);
    const float3 a = max(input.rgb, 0.0);
    const float3 b = max(output.rgb, 0.0);
    const float yi = dot(a, kLuma);
    const float yo = dot(b, kLuma);

    // Identity edit when ratios are unreliable, rather than retaining a
    // spectacular but meaningless gain from black/invalid model pixels.
    if (!all(isfinite(input)) || !all(isfinite(output)) ||
        yi < kSignalFloor || yo < kSignalFloor)
    {
        gEdit[id.xy] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float gy = log(clamp(yo / yi, 0.5, 2.0));
    const float eps = max(1.0e-4, yi * 0.01);
    const float3 raw = log((b + eps) / (a + eps));
    const float3 q = clamp(raw - gy, -kLn2, kLn2);
    const float3 tau = max(float3(kSignalFloor, kSignalFloor, kSignalFloor), yi * 0.02);
    const float3 omega = max(float3(0.01, 0.01, 0.01), yi * 0.10);
    const float3 confidence = saturate((min(a, b) - tau) / omega);
    const float3 gain = clamp(gy + confidence * q, -kLn2, kLn2);
    gEdit[id.xy] = float4(gain, 1.0);
}

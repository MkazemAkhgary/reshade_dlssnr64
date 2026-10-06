// Model input shader. Plain resample of the source into the working-scale
// buffer the model reads.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kPrepareModelInputSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
};

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

float3 LinearToSrgb(float3 v)
{
    v = max(v, 0.0);
    return lerp(v * 12.92,
                1.055 * pow(v, 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    // GainMapSplit stores the SDR base in linear light. The DLSSNR.Color
    // contract, however, is the same SDR-encoded domain used by the previous
    // HDR path (TonemapForward ended with LinearToSrgb). Keep that boundary
    // explicit here; the packed gain-map itself remains linear and lossless.
    gOutput[id.xy] = float4(LinearToSrgb(color.rgb), 1.0);
}
)";

}  // namespace reshade_dlssnr

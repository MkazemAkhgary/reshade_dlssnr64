// Model input shader. Plain resample of the source into the working-scale
// buffer the model reads.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kPrepareModelInputSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    uint  gGainMapSource;
    uint  gPad0;
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

float3 GamutFitPreserveChroma(float3 c)
{
    c = max(c, 0.0);
    float max_channel = max(c.r, max(c.g, c.b));
    if (max_channel > 1.0)
        c /= max_channel;
    return c;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    // HDR gain-map sources are a lossless linear SDR carrier and may contain
    // super-whites. Fit that carrier into the SDR model gamut without changing
    // RGB ratios, then apply the model's sRGB transfer function. The packed
    // carrier remains untouched and is what HDR compose reconstructs from.
    float3 linColor = color.rgb;
    if (gGainMapSource != 0u)
        linColor = GamutFitPreserveChroma(linColor);
    gOutput[id.xy] = float4(LinearToSrgb(linColor), 1.0);
}
)";

}  // namespace reshade_dlssnr

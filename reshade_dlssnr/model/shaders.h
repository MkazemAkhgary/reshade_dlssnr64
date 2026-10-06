// Model input preparation. Downsampling into the model working resolution
// uses exact pixel-footprint box/area averaging rather than the compute engine's
// linear sampler. The shared helper is also used by the HDR gain-map split when
// the skin detector is evaluated directly at its reduced working resolution.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kBoxAreaAveragePreludeSource = R"(
float4 BoxAreaAverage(Texture2D<float4> source,
                      uint2 dst_xy,
                      uint2 dst_size,
                      uint2 src_size)
{
    const float2 src_min = float2(dst_xy) *
                           (float2(src_size) / float2(dst_size));
    const float2 src_max = (float2(dst_xy) + 1.0) *
                           (float2(src_size) / float2(dst_size));

    const int x0 = max(0, (int)floor(src_min.x));
    const int y0 = max(0, (int)floor(src_min.y));
    const int x1 = min((int)src_size.x - 1, (int)ceil(src_max.x) - 1);
    const int y1 = min((int)src_size.y - 1, (int)ceil(src_max.y) - 1);

    float4 accum = 0.0;
    float total_weight = 0.0;

    for (int y = y0; y <= y1; ++y) {
        const float wy = max(0.0,
                             min(src_max.y, (float)y + 1.0) -
                             max(src_min.y, (float)y));
        if (wy <= 0.0)
            continue;

        for (int x = x0; x <= x1; ++x) {
            const float wx = max(0.0,
                                 min(src_max.x, (float)x + 1.0) -
                                 max(src_min.x, (float)x));
            if (wx <= 0.0)
                continue;

            const float weight = wx * wy;
            accum += source.Load(int3(x, y, 0)) * weight;
            total_weight += weight;
        }
    }

    return accum / max(total_weight, 1e-8);
}
)";

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

    // Exact area average over the source footprint covered by this working
    // pixel. This is the downsampling filter for both SDR and HDR carrier
    // inputs; it does not depend on the global linear-clamp sampler.
    uint src_width = 0;
    uint src_height = 0;
    gSource.GetDimensions(src_width, src_height);
    float4 color = BoxAreaAverage(gSource, id.xy, uint2(gWidth, gHeight),
                                  uint2(src_width, src_height));
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

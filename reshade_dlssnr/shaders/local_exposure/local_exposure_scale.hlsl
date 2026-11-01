// Per-pixel local-exposure multiplier for a display-referred source.
//
// The HDR path evaluates the same operator inside the gain-map split, because
// that split already carries the tone curve and the exposure that define its
// display luminance. A source that is already display-referred needs neither,
// so the operator gets the source's own luminance and runs here instead.
//
// The optional second output writes the image lifted by that multiplier, which
// is what the model input needs.

#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"

Texture2D<float4> gSource : register(t0);
RWTexture2D<float> gScale : register(u0);
RWTexture2D<float4> gLifted : register(u1);
SamplerState s0 : register(s0);

#define kLocalExposureSdrSource 1
#include "common/hdr_local_exposure.hlsli"

#define kWriteLifted shader.Params0.x

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    const float3 encoded = gSource.Load(int3(id.xy, 0)).rgb;
    const float3 linear_rgb = SrgbToLinear(encoded);
    const float2 uv = (float2(id.xy) + 0.5) /
                      float2(max(Width, 1u), max(Height, 1u));

    // A display-referred pixel has no room above white, so the multiplier is
    // bounded by the headroom the pixel has left. The bound is taken on the
    // largest channel so the colour keeps its ratios. The map is what the
    // restore divides by, so it has to be the multiplier actually applied.
    const float max_channel = max(linear_rgb.r,
                                  max(linear_rgb.g, linear_rgb.b));
    const float scale = min(LocalExposureScale(uv, SourceSdrLuma(encoded)),
                            1.0 / max(max_channel, 1e-4));
    gScale[id.xy] = scale;

    if (kWriteLifted > 0.5)
    {
        gLifted[id.xy] = float4(LinearToSrgb(linear_rgb * scale), 1.0);
    }
}

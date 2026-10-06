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

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 color = gSource.SampleLevel(s0, uv, 0);
    gOutput[id.xy] = float4(color.rgb, 1.0);
}
)";

}  // namespace reshade_dlssnr

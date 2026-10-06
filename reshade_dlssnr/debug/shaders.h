// Debug view display shader. Copies a pipeline resource to the debug output,
// with per-mode channel remapping.
#pragma once

namespace reshade_dlssnr {

inline constexpr const char* kDebugDisplaySource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[12];
};

// gParams[0].x = display mode (0 color, 1 motion, 2 depth, 3 mask, 4 packed SDR, 5 gain)
// gParams[0].y = source content width
// gParams[0].z = source content height
// gParams[0].w = source resource width
// gParams[1].x = source resource height
#define kMode          gParams[0].x
#define kContentW      gParams[0].y
#define kContentH      gParams[0].z
#define kResourceW     gParams[0].w
#define kResourceH     gParams[1].x

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight) return;

    float2 dstUv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float2 srcUv = dstUv * float2(kContentW / kResourceW,
                                  kContentH / kResourceH);

    float4 c = gSource.SampleLevel(s0, srcUv, 0);

    int mode = (int)(kMode + 0.5);
    float3 rgb;
    if (mode == 1) {
        rgb = float3(0.5 + 0.5 * c.x, 0.5 + 0.5 * c.y, 0.0);
    } else if (mode == 2 || mode == 3) {
        rgb = c.rrr;
    } else if (mode == 4) {
        // Packed HDR gain-map RGB is linear SDR. Display it directly; ReShade
        // will perform the final presentation transform.
        rgb = c.rgb;
    } else if (mode == 5) {
        // Diagnostic-only mapping: raw log2 gain [-4, +6] -> [0, 1].
        float g = saturate((c.a + 4.0) / 10.0);
        rgb = g.xxx;
    } else {
        rgb = c.rgb;
    }

    gOutput[id.xy] = float4(rgb, 1.0);
}
)";

}  // namespace reshade_dlssnr

#include "common/cbuffer.hlsli"
#include "common/registers.hlsli"

// Debug-only parameters live in the generic shader block for this pass.
#define kMode          shader.Params0.x
#define kContentW      shader.Params0.y
#define kContentH      shader.Params0.z
#define kResourceW     shader.Params0.w
#define kResourceH     shader.Params1.x

Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    float2 dstUv = (float2(id.xy) + 0.5) / float2(Width, Height);
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

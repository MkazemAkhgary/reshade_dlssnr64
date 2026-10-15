#include "common/bindings.hlsli"
#include "common/registers.hlsli"

// Debug-only parameters live in the generic shader block for this pass.
#define kMode          shader.Params0.x
#define kContentW      shader.Params0.y
#define kContentH      shader.Params0.z
#define kResourceW     shader.Params0.w
#define kResourceH     shader.Params1.x
#define kGameExposure  shader.Params1.y
#define kGameExposureAvailable shader.Params1.z

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
        // Alpha stores the carrier-to-HDR reconstruction multiplier; display its log2
        // value as stops, mapped from [-4, +6] to [0, 1].
        float stops = log2(max(c.a, 1.0e-20));
        float g = saturate((stops + 4.0) / 10.0);
        rgb = g.xxx;
    } else if (mode == 9) {
        // Exposure is shown in stops; magenta marks unavailable data.
        if (kGameExposureAvailable < 0.5) {
            rgb = float3(1.0, 0.0, 1.0);
        } else {
            float stops = log2(max(kGameExposure, 1.0e-6));
            float t = saturate((stops + 4.0) / 8.0);
            rgb = t.xxx;
        }
    } else if (mode == 10) {
        // Candidate alpha: 0 = rejected, 1 = eligible, 2 = photometric mismatch.
        rgb = kTemporalDebugAvailable < 0.5 ? float3(0.04, 0.12, 0.55) :
              c.a < 0.5 ? float3(0.85, 0.04, 0.04) :
              c.a < 1.5 ? float3(0.04, 0.80, 0.10) :
                          float3(1.00, 0.60, 0.02);
    } else if (mode == 11) {
        // Validated alpha: 0 = no candidate, 1 = accepted, 2 = photo mismatch,
        // 3 = depth/guide rejection.
        rgb = kTemporalDebugAvailable < 0.5 ? float3(0.04, 0.12, 0.55) :
              c.a < 0.5 ? float3(0.85, 0.04, 0.04) :
              c.a < 1.5 ? float3(0.04, 0.80, 0.10) :
              c.a < 2.5 ? float3(1.00, 0.60, 0.02) :
                          float3(1.00, 0.02, 0.85);
    } else if (mode == 12) {
        // Per-channel log gain: neutral is gray; invalid pixels are dark.
        rgb = kTemporalDebugAvailable < 0.5 ? float3(0.04, 0.12, 0.55) :
              (c.a >= 0.5 && c.a < 2.5)
            ? saturate(0.5 + c.rgb / (2.0 * 0.69314718056))
            : float3(0.03, 0.00, 0.03);
    } else {
        rgb = c.rgb;
    }

    gOutput[id.xy] = float4(rgb, 1.0);
}

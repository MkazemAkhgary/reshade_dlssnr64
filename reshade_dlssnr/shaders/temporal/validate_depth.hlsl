#include "common/bindings.hlsli"

Texture2D<float4> gCandidate : register(t0);
Texture2D<float2> gMotion : register(t1);
Texture2D<float> gCurrentDepth : register(t2);
Texture2D<float> gPreviousDepth : register(t3);
RWTexture2D<float4> gValidated : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;
    const float4 candidate = gCandidate.Load(int3(id.xy, 0));
    if (candidate.a < 0.5)
    {
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    uint mw, mh, cw, ch;
    gMotion.GetDimensions(mw, mh);
    gCurrentDepth.GetDimensions(cw, ch);
    if (mw == 0 || mh == 0 || cw == 0 || ch == 0)
    {
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 3.0);
        return;
    }
    const float2 motion_base = float2(shader.Params0.z, shader.Params0.w);
    const float2 motion_size = float2(shader.Params1.z, shader.Params1.w);
    const float2 motion_pixel = motion_base + uv * motion_size;
    if (any(motion_pixel < 0.0) || any(motion_pixel >= float2(mw, mh)))
    {
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const uint2 mxy = min(uint2(motion_pixel), uint2(mw - 1u, mh - 1u));
    const float2 mv = gMotion.Load(int3(mxy, 0));
    const float2 reference = max(float2(shader.Params1.x, shader.Params1.y), 1.0);
    const float2 previous_uv = uv + mv * float2(shader.Params0.x, shader.Params0.y) / reference;
    if (!all(isfinite(mv)) || !all(isfinite(previous_uv)) ||
        any(previous_uv < 0.0) || any(previous_uv >= 1.0))
    {
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }
    const float2 guide_base = hdr.LocalExposure.xy;
    const float2 guide_size = hdr.LocalExposure.zw;
    const float2 current_depth_pixel = guide_base + uv * guide_size;
    if (any(current_depth_pixel < 0.0) ||
        any(current_depth_pixel >= float2(cw, ch)))
    {
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 3.0);
        return;
    }
    const uint2 cxy = min(uint2(current_depth_pixel), uint2(cw - 1u, ch - 1u));
    const uint2 pxy = min(uint2(previous_uv * float2(Width, Height)),
                          uint2(Width - 1u, Height - 1u));
    const float d_now = gCurrentDepth.Load(int3(cxy, 0));
    const float d_old = gPreviousDepth.Load(int3(pxy, 0));
    const float threshold = max(0.005, 0.03 * max(abs(d_now), abs(d_old)));
    if (!isfinite(d_now) || !isfinite(d_old) || abs(d_now - d_old) > threshold)
    {
        // Depth-invalid edits are not applied; identity is committed as valid
        // history after the frame to avoid a stuck history-validity hole.
        gValidated[id.xy] = float4(0.0, 0.0, 0.0, 3.0);
        return;
    }
    gValidated[id.xy] = float4(clamp(candidate.rgb, -0.69314718056, 0.69314718056),
                               candidate.a >= 1.5 ? 2.0 : 1.0);
}

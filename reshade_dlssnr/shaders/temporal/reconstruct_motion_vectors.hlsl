#include "common/bindings.hlsli"

Texture2D<float4> gCurrentInput : register(t0);
Texture2D<float4> gPreviousInput : register(t1);
Texture2D<float> gCurrentDepth : register(t2);
Texture2D<float> gPreviousDepth : register(t3);
RWTexture2D<float2> gMotion : register(u0);
RWTexture2D<float> gCurrentDepthReduced : register(u1);

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);
static const float kInvalidCost = 1.0e6;

float ReadCurrentDepth(uint2 pixel, uint depth_width, uint depth_height)
{
    float result = 0.0;
    if (depth_width > 0u && depth_height > 0u) {
        const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
        const float2 source_pixel = kTemporalGuideBase + uv * kTemporalGuideSize;
        if (all(source_pixel >= 0.0) &&
            all(source_pixel < float2(depth_width, depth_height))) {
            const uint2 source_xy = min(uint2(source_pixel),
                                        uint2(depth_width - 1u, depth_height - 1u));
            const float depth = gCurrentDepth.Load(int3(source_xy, 0));
            result = isfinite(depth) ? depth : 0.0;
        }
    }
    return result;
}

float CandidateCost(int2 candidate_pixel, float current_log_luma,
                    float current_depth)
{
    float cost = kInvalidCost;
    if (all(candidate_pixel >= 0) &&
        all(candidate_pixel < int2(Width, Height))) {
        const uint2 p = uint2(candidate_pixel);
        const float3 previous_rgb = max(gPreviousInput.Load(int3(p, 0)).rgb, 0.0);
        const float previous_luma = dot(previous_rgb, kLumaWeights);
        if (isfinite(previous_luma)) {
            const float previous_log_luma = log2(max(previous_luma, 1.0e-4));
            cost = abs(current_log_luma - previous_log_luma);
            const float previous_depth = gPreviousDepth.Load(int3(p, 0));
            if (current_depth > 0.0 && previous_depth > 0.0 &&
                isfinite(current_depth) && isfinite(previous_depth)) {
                const float depth_delta = abs(current_depth - previous_depth);
                const float depth_reference = max(abs(current_depth), abs(previous_depth));
                const float depth_tolerance = max(kTemporalDepthAbsThreshold,
                    kTemporalDepthRelThreshold * depth_reference);
                if (depth_delta > depth_tolerance)
                    cost = kInvalidCost;
                else
                    cost += 0.35 * depth_delta / max(depth_tolerance, 1.0e-5);
            }
        }
    }
    return cost;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    uint depth_width, depth_height;
    gCurrentDepth.GetDimensions(depth_width, depth_height);
    const float current_depth = ReadCurrentDepth(id.xy, depth_width, depth_height);
    gCurrentDepthReduced[id.xy] = current_depth;

    float2 motion = float2(0.0, 0.0);
    if (kTemporalUseValidatedHistory > 0.5 && current_depth > 0.0)
    {
        const float3 current_rgb = max(gCurrentInput.Load(int3(id.xy, 0)).rgb, 0.0);
        const float current_luma = dot(current_rgb, kLumaWeights);
        if (isfinite(current_luma))
        {
            const float current_log_luma = log2(max(current_luma, 1.0e-4));
            int2 best = int2(0, 0);
            float best_cost = CandidateCost(int2(id.xy), current_log_luma,
                                           current_depth);

            // Coarse-to-fine 2D search. The depth term rejects matches across
            // silhouettes and the photometric term resolves motion along them.
            [unroll]
            for (int cy = -1; cy <= 1; ++cy)
            {
                [unroll]
                for (int cx = -1; cx <= 1; ++cx)
                {
                    const int2 candidate = int2(id.xy) + int2(cx, cy) * 4;
                    const float cost = CandidateCost(candidate, current_log_luma,
                                                    current_depth);
                    if (cost < best_cost)
                    {
                        best_cost = cost;
                        best = int2(cx, cy) * 4;
                    }
                }
            }
            [unroll]
            for (int ry = -1; ry <= 1; ++ry)
            {
                [unroll]
                for (int rx = -1; rx <= 1; ++rx)
                {
                    const int2 candidate_offset = best + int2(rx, ry) * 2;
                    const float cost = CandidateCost(int2(id.xy) + candidate_offset,
                                                    current_log_luma, current_depth);
                    if (cost < best_cost)
                    {
                        best_cost = cost;
                        best = candidate_offset;
                    }
                }
            }
            [unroll]
            for (int fy = -1; fy <= 1; ++fy)
            {
                [unroll]
                for (int fx = -1; fx <= 1; ++fx)
                {
                    const int2 candidate_offset = best + int2(fx, fy);
                    const float cost = CandidateCost(int2(id.xy) + candidate_offset,
                                                    current_log_luma, current_depth);
                    if (cost < best_cost)
                    {
                        best_cost = cost;
                        best = candidate_offset;
                    }
                }
            }

            // Ambiguous or newly disoccluded pixels fail toward zero motion;
            // later temporal validation remains fail-open to fresh evaluation.
            if (best_cost < 0.75)
                motion = float2(best);
        }
    }

    gMotion[id.xy] = motion;
}

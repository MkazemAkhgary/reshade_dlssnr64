#include "common/bindings.hlsli"

Texture2D<float4> gCurrentInput   : register(t0);
Texture2D<float4> gPreviousInput  : register(t1);
Texture2D<float>  gCurrentDepth   : register(t2);
Texture2D<float>  gPreviousDepth  : register(t3);
Texture2D<float2> gGameMotion     : register(t4);
Texture2D<float2> gPreviousMotion : register(t5);
RWTexture2D<float2> gMotion       : register(u0);
RWTexture2D<float>  gCurrentDepthReduced : register(u1);

static const float3 kLumaWeights = float3(0.2126, 0.7152, 0.0722);
static const float kInvalidCost = 1.0e6;
// A match this good is trusted; anything worse keeps the seed vector.
static const float kAcceptCost = 0.75;

// The output field is in working-resolution pixels: the caller binds it with
// scale 1 and its own size as the reference, so a displacement here is a
// displacement in this dispatch's pixels.

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

// The game's vector for this pixel, converted from reference pixels to this
// dispatch's pixels. Returns whether the texture had data for the pixel.
float2 ReadGameMotion(uint2 pixel, out bool out_found)
{
    float2 result = float2(0.0, 0.0);
    out_found = false;
    uint motion_width = 0u;
    uint motion_height = 0u;
    gGameMotion.GetDimensions(motion_width, motion_height);
    if (motion_width > 0u && motion_height > 0u) {
        const float2 uv = (float2(pixel) + 0.5) / float2(Width, Height);
        const float2 source_pixel =
            kTemporalMotionBase + uv * kTemporalMotionSubrectSize;
        if (all(source_pixel >= 0.0) &&
            all(source_pixel < float2(motion_width, motion_height))) {
            const uint2 source_xy = min(uint2(source_pixel),
                                        uint2(motion_width - 1u, motion_height - 1u));
            const float2 raw = gGameMotion.Load(int3(source_xy, 0));
            if (all(isfinite(raw))) {
                const float2 reference =
                    max(kTemporalMotionReferenceSize, float2(1.0, 1.0));
                result = raw * kTemporalMotionScale / reference *
                         float2(Width, Height);
                out_found = true;
            }
        }
    }
    return result;
}

float2 ReadPreviousMotion(uint2 pixel, out bool out_found)
{
    float2 result = float2(0.0, 0.0);
    out_found = false;
    uint motion_width = 0u;
    uint motion_height = 0u;
    gPreviousMotion.GetDimensions(motion_width, motion_height);
    if (motion_width > 0u && motion_height > 0u) {
        const uint2 source_xy =
            min(pixel, uint2(motion_width - 1u, motion_height - 1u));
        const float2 raw = gPreviousMotion.Load(int3(source_xy, 0));
        if (all(isfinite(raw))) {
            result = raw;
            out_found = true;
        }
    }
    return result;
}

// Photometric and depth cost of a displacement. A candidate that lands off the
// image, or on a different surface, is rejected outright.
float CandidateCost(uint2 pixel, float2 offset, float current_log_luma,
                    float current_depth)
{
    float cost = kInvalidCost;
    const int2 candidate_pixel = int2(pixel) + int2(round(offset));
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

// Prefers the previous field when two displacements explain the image equally
// well, so a corrected vector does not wander between frames. Normalised by
// the previous length: a one-pixel change is a small step for a long vector
// and a large one for a short vector.
float PriorCost(float2 offset, float2 prior, bool have_prior)
{
    float result = 0.0;
    if (have_prior) {
        result = kTemporalMotionPriorWeight * length(offset - prior) /
                 (1.0 + length(prior));
    }
    return result;
}

// One coarse-to-fine level: the eight neighbours of `base` at `radius`, plus
// `base` itself through the caller's current best cost.
float2 Refine(uint2 pixel, float2 base, int radius, float current_log_luma,
              float current_depth, float2 prior, bool have_prior,
              inout float best_cost)
{
    float2 best = base;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            const float2 candidate = base + float2(dx, dy) * float(radius);
            const float cost =
                CandidateCost(pixel, candidate, current_log_luma, current_depth) +
                PriorCost(candidate, prior, have_prior);
            if (cost < best_cost) {
                best_cost = cost;
                best = candidate;
            }
        }
    }
    return best;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height) return;

    uint depth_width, depth_height;
    gCurrentDepth.GetDimensions(depth_width, depth_height);
    const float current_depth = ReadCurrentDepth(id.xy, depth_width, depth_height);
    gCurrentDepthReduced[id.xy] = current_depth;

    const int mode = (int)(kTemporalMotionMode + 0.5);

    // The first frame with a game field has nothing to correct against, so the
    // field is recorded as the seed for the next one.
    bool game_found = false;
    const float2 game_motion = ReadGameMotion(id.xy, game_found);
    if (mode == 2) {
        gMotion[id.xy] = game_found ? game_motion : float2(0.0, 0.0);
        return;
    }

    float2 motion = float2(0.0, 0.0);
    if (current_depth > 0.0) {
        const float3 current_rgb = max(gCurrentInput.Load(int3(id.xy, 0)).rgb, 0.0);
        const float current_luma = dot(current_rgb, kLumaWeights);
        if (isfinite(current_luma)) {
            const float current_log_luma = log2(max(current_luma, 1.0e-4));

            bool prior_found = false;
            float2 prior = float2(0.0, 0.0);
            if (kTemporalUseValidatedHistory > 0.5) {
                prior = ReadPreviousMotion(id.xy, prior_found);
            }

            // Correction starts from the game's vector, and synthesis starts
            // from the previous field. In both cases the previous field pulls
            // the result toward itself, which keeps the output stable over time.
            //
            // A pixel the guide had no data for is synthesized, not corrected.
            const bool correcting = mode == 1 && game_found;
            const float2 seed = correcting
                ? game_motion
                : (prior_found ? prior : float2(0.0, 0.0));
            const bool have_prior = prior_found;

            float best_cost = CandidateCost(id.xy, seed, current_log_luma,
                                            current_depth) +
                              PriorCost(seed, prior, have_prior);
            float2 best = seed;
            if (correcting) {
                // The game's vector is already close; the radius bounds how far
                // the correction may pull it.
                const int coarse = max((int)(kTemporalMotionRadius + 0.5), 1);
                if (coarse > 1) {
                    best = Refine(id.xy, best, coarse, current_log_luma,
                                  current_depth, prior, have_prior, best_cost);
                }
            } else {
                // Fixed 4/2/1 ladder over the whole neighbourhood: there is no
                // guide, so the search has to cover the displacement itself.
                best = Refine(id.xy, best, 4, current_log_luma, current_depth,
                              prior, have_prior, best_cost);
                best = Refine(id.xy, best, 2, current_log_luma, current_depth,
                              prior, have_prior, best_cost);
            }
            best = Refine(id.xy, best, 1, current_log_luma, current_depth, prior,
                          have_prior, best_cost);

            // Ambiguous or newly disoccluded pixels keep the seed: zero motion
            // when synthesising, the game's own vector when correcting. Later
            // temporal validation stays fail-open to a fresh evaluation.
            if (best_cost < kAcceptCost) motion = best;
            else if (correcting) motion = game_motion;
        }
    }

    gMotion[id.xy] = motion;
}

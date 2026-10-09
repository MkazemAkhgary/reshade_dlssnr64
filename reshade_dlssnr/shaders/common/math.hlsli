#pragma once
#ifndef DLSSNR_MATH_HLSLI
#define DLSSNR_MATH_HLSLI

#include "bindings.hlsli"


float SoftCompress(float x, float max_pos, float max_neg, float knee)
{
    knee = max(knee, 1e-6);
    max_pos = max(max_pos, 1e-6);
    max_neg = max(max_neg, 1e-6);
    if (x >= 0.0)
        return max_pos * x / (max_pos + knee * abs(x));
    else
        return max_neg * x / (max_neg + knee * abs(x));
}

float safe_log(float x)
{
    return log(max(x, 0.0) + max(kRatioEps, 1e-8));
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + max(kRatioEps, 1e-8));
}


#endif

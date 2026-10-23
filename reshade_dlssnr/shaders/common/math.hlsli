#pragma once

#include "bindings.hlsli"

// Natural log of 2: the log-gain <-> linear-gain conversion constant.
static const float kLn2 = 0.69314718056;


float safe_log(float x)
{
    return log(max(x, 0.0) + max(kRatioEps, 1e-8));
}

float3 safe_log3(float3 v)
{
    return log(max(v, 0.0) + max(kRatioEps, 1e-8));
}

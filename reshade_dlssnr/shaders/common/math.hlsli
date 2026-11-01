#pragma once

#include "bindings.hlsli"

// Natural log of 2: the log-gain <-> linear-gain conversion constant.
static const float kLn2 = 0.69314718056;


// Floor, do not add. Adding the epsilon to both endpoints biases their ratio
// wherever a channel sits near it, and the bias is per channel because channels
// differ in magnitude: the largest channel keeps its ratio and the others lose
// part of theirs, so a dark pixel shifts toward its dominant channel. Flooring
// leaves the ratio exact until a channel is actually below the floor.
float safe_log(float x)
{
    return log(max(x, max(kRatioEps, 1e-8)));
}

float3 safe_log3(float3 v)
{
    return log(max(v, max(kRatioEps, 1e-8)));
}

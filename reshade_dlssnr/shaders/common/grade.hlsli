#pragma once

#include "bindings.hlsli"


static const float kGradeEps = 1e-6;

float GradeLevelSpan()
{
    // Keep old/corrupt persisted settings monotone even if black >= white.
    float white = max(kGradeWhiteLevel,
                      kGradeBlackLevel + 1e-4);
    return white - kGradeBlackLevel;
}

float GradeLevelsFwd(float y)
{
    return (y - kGradeBlackLevel) / GradeLevelSpan();
}

float GradeLevelsInv(float y)
{
    return y * GradeLevelSpan() + kGradeBlackLevel;
}

// -----------------------------------------------------------------------------
// Contrast
// -----------------------------------------------------------------------------

float GradeContrastFwd(float y)
{
    float result = y;
    if (kGradeContrast != 0.0)
        result = (y - 0.5) * exp2(kGradeContrast) + 0.5;
    return result;
}

float GradeContrastInv(float y)
{
    float result = y;
    if (kGradeContrast != 0.0)
        result = (y - 0.5) / exp2(kGradeContrast) + 0.5;
    return result;
}

// -----------------------------------------------------------------------------
// Shadow/highlight shaping
//
// These replace the old exponential curves.
//
// Shadow:
//   y + 0.5*a*y*(1-y)^2
//
// Highlight:
//   y + 0.5*a*y^2*(1-y)
//
// The saturate() terms make the actual modification disappear outside the
// normalized [0,1] region instead of creating strange HDR behavior.
//
// With a in [-1,1], both mappings are monotone and have positive derivative,
// so an inverse exists.
// -----------------------------------------------------------------------------

float GradeShadowFwd(float y, float a)
{
    float result = y;
    if (a != 0.0)
    {
        float t = saturate(y);
        float room = 1.0 - t;
        result = y + 0.5 * a * y * room * room;
    }
    return result;
}

float GradeShadowInv(float y_out, float a)
{
    float result = y_out;
    if (a != 0.0)
    {
        if (y_out <= 0.0)
        {
            result = y_out / (1.0 + 0.5 * a);
        }
        else if (y_out < 1.0)
        {
            float y = saturate(y_out);
            [unroll]
            for (int i = 0; i < 6; ++i)
            {
                float room = 1.0 - y;
                float f = y + 0.5 * a * y * room * room - y_out;
                float df = 1.0 + 0.5 * a *
                           (room * room - 2.0 * y * room);
                y -= f / max(abs(df), kGradeEps) * sign(df);
                y = saturate(y);
            }
            result = y;
        }
    }
    return result;
}

float GradeHighlightFwd(float y, float a)
{
    float result = y;
    if (a != 0.0)
    {
        float t = saturate(y);
        result = y + 0.5 * a * t * t * (1.0 - y);
    }
    return result;
}

float GradeHighlightInv(float y_out, float a)
{
    float result = y_out;
    if (a != 0.0)
    {
        if (y_out >= 1.0)
        {
            result = (y_out - 0.5 * a) / (1.0 - 0.5 * a);
        }
        else if (y_out > 0.0)
        {
            float y = saturate(y_out);
            [unroll]
            for (int i = 0; i < 6; ++i)
            {
                float f = y + 0.5 * a * y * y * (1.0 - y) - y_out;
                float df = 1.0 + 0.5 * a *
                           (2.0 * y * (1.0 - y) - y * y);
                y -= f / max(abs(df), kGradeEps) * sign(df);
                y = saturate(y);
            }
            result = y;
        }
    }
    return result;
}

// -----------------------------------------------------------------------------
// Luma modes
// -----------------------------------------------------------------------------

float SignedPow(float y, float p)
{
    float result = 0.0;
    if (y != 0.0)
        result = (y < 0.0 ? -1.0 : 1.0) * pow(abs(y), p);
    return result;
}

float GradeLumaFwd(float y, float a, int mode)
{
    float result = y;
    if (a != 0.0)
    {
        if (mode == 0)
        {
            result = y * exp2(a);
        }
        else if (mode == 1)
        {
            result = SignedPow(y, exp2(-a));
        }
        else if (mode == 2)
        {
            result = y * exp2(a);
        }
        else
        {
            float t = saturate(y);
            result = y + 0.5 * a * t * (1.0 - t);
        }
    }
    return result;
}

float GradeLumaInv(float y, float a, int mode)
{
    float result = y;
    if (a != 0.0)
    {
        if (mode == 0)
        {
            result = y / exp2(a);
        }
        else if (mode == 1)
        {
            result = SignedPow(y, exp2(a));
        }
        else if (mode == 2)
        {
            result = y / exp2(a);
        }
        else if (y > 0.0 && y < 1.0)
        {
            float x = saturate(y);
            [unroll]
            for (int i = 0; i < 6; ++i)
            {
                float f = x + 0.5 * a * x * (1.0 - x) - y;
                float df = 1.0 + 0.5 * a * (1.0 - 2.0 * x);
                x -= f / max(abs(df), kGradeEps) * sign(df);
                x = saturate(x);
            }
            result = x;
        }
    }
    return result;
}

// -----------------------------------------------------------------------------
// Chroma
// -----------------------------------------------------------------------------

float3 GradeChromaSaturationFwd(float3 c, float a)
{
    float y = dot(c, kL709);
    float g = 1.0 + a;

    return y + (c - y) * g;
}

float3 GradeChromaSaturationInv(float3 c, float a)
{
    float y = dot(c, kL709);
    float g = max(1.0 + a, kGradeEps);

    return y + (c - y) / g;
}

// Vibrance / smart saturation.
//
// Chroma gain is based on chroma magnitude relative to luminance, then
// progressively falls to zero as the colour is already saturated.  This makes
// Vibrance materially different from uniform Saturation while keeping the
// transform invertible for the inverse-tone-map path.

float GradeVibranceSaturation(float3 c)
{
    float y = dot(c, kL709);
    float chroma = length(c - y.xxx);
    return chroma / max(abs(y), kGradeEps);
}

float GradeVibranceGain(float saturation, float a)
{
    float muted = 1.0 - saturate(saturation);
    return 1.0 + a * muted * muted;
}

float3 GradeChromaVibranceFwd(float3 c, float a)
{
    float y = dot(c, kL709);
    float saturation = GradeVibranceSaturation(c);
    float g = GradeVibranceGain(saturation, a);
    return y + (c - y) * g;
}

float3 GradeChromaVibranceInv(float3 c, float a)
{
    float3 result = c;
    float y = dot(c, kL709);
    float output_saturation = GradeVibranceSaturation(c);

    // Once the input is at or above the smart-saturation knee, the forward
    // gain is exactly 1, so no solve is required.
    if (!(output_saturation >= 1.0))
    {
        // Solve s_out = s * (1 + a * (1 - s)^2) for the original saturation s.
        float s = saturate(output_saturation);
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            float muted = 1.0 - s;
            float gain = 1.0 + a * muted * muted;
            float f = s * gain - output_saturation;
            float df = 1.0 + a * (1.0 - 4.0 * s + 3.0 * s * s);
            s -= f / max(abs(df), kGradeEps) * sign(df);
            s = saturate(s);
        }

        float g = max(GradeVibranceGain(s, a), kGradeEps);
        result = y + (c - y) / g;
    }
    return result;
}

float3 GradeChromaFwd(float3 c, float a, int mode)
{
    float3 result = c;
    if (a != 0.0)
    {
        if (mode == 0)
            result = GradeChromaSaturationFwd(c, a);
        else
            result = GradeChromaVibranceFwd(c, a);
    }
    return result;
}

float3 GradeChromaInv(float3 c, float a, int mode)
{
    float3 result = c;
    if (a != 0.0)
    {
        if (mode == 0)
            result = GradeChromaSaturationInv(c, a);
        else
            result = GradeChromaVibranceInv(c, a);
    }
    return result;
}

// -----------------------------------------------------------------------------
// Linear-domain grade core
// -----------------------------------------------------------------------------

float3 GradeLinearCoreFwd(float3 lin)
{
    const float initial_luma = dot(lin, kL709);
    float target_luma = initial_luma;

    target_luma = GradeLevelsFwd(target_luma);
    target_luma = GradeContrastFwd(target_luma);
    target_luma = GradeShadowFwd(target_luma, kGradeShadows);
    target_luma = GradeHighlightFwd(target_luma, kGradeHighlights);

    const int brightness_mode = (int)(kGradeLumaMode + 0.5);
    float3 color = lin + (target_luma - initial_luma);
    if (brightness_mode == 2)
    {
        // Exposure is a linear-light scale, not a luminance offset.
        color *= exp2(kGradeLuma);
    }
    else
    {
        target_luma = GradeLumaFwd(target_luma, kGradeLuma, brightness_mode);
        color = lin + (target_luma - initial_luma);
    }

    return GradeChromaFwd(
        color,
        kGradeChroma,
        (int)(kGradeChromaMode + 0.5)
    );
}

float3 GradeLinearCoreInv(float3 lin)
{
    float3 color = GradeChromaInv(
        lin,
        kGradeChroma,
        (int)(kGradeChromaMode + 0.5)
    );

    const int brightness_mode = (int)(kGradeLumaMode + 0.5);
    if (brightness_mode == 2)
    {
        color /= exp2(kGradeLuma);
    }

    float target_luma = dot(color, kL709);
    if (brightness_mode != 2)
    {
        target_luma = GradeLumaInv(target_luma, kGradeLuma, brightness_mode);
    }

    target_luma = GradeHighlightInv(target_luma, kGradeHighlights);
    target_luma = GradeShadowInv(target_luma, kGradeShadows);
    target_luma = GradeContrastInv(target_luma);
    target_luma = GradeLevelsInv(target_luma);

    const float restored_current_luma = dot(color, kL709);
    return color + (target_luma - restored_current_luma);
}

// -----------------------------------------------------------------------------
// Public linear entry points
// -----------------------------------------------------------------------------

float3 GradeLinearFwd(float3 lin)
{
    float3 result = lin;
    if (kGradeEnabled >= 0.5)
        result = GradeLinearCoreFwd(lin);
    return result;
}

float3 GradeLinearInv(float3 lin)
{
    float3 result = lin;
    if (kGradeEnabled >= 0.5)
        result = GradeLinearCoreInv(lin);
    return result;
}

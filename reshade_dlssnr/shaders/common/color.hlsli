#pragma once

#include "color_constants.h"

float3 SrgbToLinear(float3 v)
{
    v = saturate(v);
    return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4),
                step(0.04045, v));
}

float3 SrgbToLinearExt(float3 v)
{
    float3 av = abs(v);
    float3 lo = av / 12.92;
    float3 hi = pow((av + 0.055) / 1.055, 2.4);
    float3 lin = lerp(lo, hi, step(0.04045, av));
    return sign(v) * lin;
}

float3 LinearToSrgbExt(float3 v)
{
    float3 av = abs(v);
    float3 lo = av * 12.92;
    float3 hi = 1.055 * pow(av, 1.0 / 2.4) - 0.055;
    float3 srgb = lerp(lo, hi, step(0.0031308, av));
    return sign(v) * srgb;
}

float3 PqToNits(float3 v)
{
    v = saturate(v);
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(v, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return kPqPeakNits * pow(num / max(den, 1e-6), 1.0 / m1);
}

float3 NitsToPq(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 Y = max(nits / kPqPeakNits, 0.0);
    float3 Ym1 = pow(Y, m1);
    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;
    return pow(num / max(den, 1e-6), m2);
}

float3 LinearToSrgb(float3 v)
{
    v = max(v, 0.0);
    return lerp(v * 12.92,
                1.055 * pow(v, 1.0 / 2.4) - 0.055,
                step(0.0031308, v));
}


float3 DecodeToLinear(float3 c, int format_code, float reference_nits)
{
    float3 result = SrgbToLinear(c);
    int fmt = format_code;
    if (fmt == 1)
        result = max(c, 0.0) *
                 (kScRgbReferenceWhiteNits / max(reference_nits, 1e-6));
    else if (fmt == 2)
        result = max(PqToNits(c), 0.0) / max(reference_nits, 1e-6);
    return result;
}

float ModelGamutCompressMax(float max_channel)
{
    float result = max_channel;
    if (max_channel > kModelGamutCompressionStart)
    {
        if (max_channel <= 1.0)
        {
            const float t = (max_channel - kModelGamutCompressionStart) /
                            max(1.0 - kModelGamutCompressionStart, 1e-6);
            result = lerp(kModelGamutCompressionStart,
                          kModelGamutCompressionWhite, saturate(t));
        }
        else
        {
            // Super-whites stay inside the model's valid input gamut. The logarithmic
            // map is invertible, while packed.a retains the full scene-referred scale.
            const float value_log = log2(max_channel);
            const float reference_log = log2(kModelGamutReferenceLinear);
            if (max_channel <= kModelGamutReferenceLinear)
            {
                const float t = value_log / max(reference_log, 1e-6);
                result = lerp(kModelGamutCompressionWhite,
                              kModelGamutReferenceMappedWhite, saturate(t));
            }
            else
            {
                // Continue the log curve beyond FP16/scRGB headroom rather than
                // clamping the input or HDR output.
                const float extended_log = log2(kModelGamutExtendedLinear);
                const float tail_slope =
                    (kModelGamutExtendedMappedWhite - kModelGamutReferenceMappedWhite) /
                    max(extended_log - reference_log, 1e-6);
                result = kModelGamutReferenceMappedWhite +
                         (value_log - reference_log) * tail_slope;
            }
        }
    }
    return result;
}

float3 GamutFitModelInput(float3 c)
{
    c = max(c, 0.0);
    float3 result = c;
    const float max_channel = max(c.r, max(c.g, c.b));
    if (!(max_channel <= kModelGamutCompressionStart))
    {
        // Map super-whites into the model's valid sub-white domain. The HDR source
        // magnitude remains losslessly represented by the independent gain scale.
        const float desat = saturate(
            kModelHighlightDesaturation *
            (max_channel - kModelGamutHighlightDesaturationStart) /
            max(max_channel + 1.0, 1e-6));
        const float3 white_pulled = lerp(c, max_channel.xxx, desat);
        const float compressed_max = ModelGamutCompressMax(max_channel);
        result = white_pulled * (compressed_max / max(max_channel, 1e-6));
    }
    return result;
}

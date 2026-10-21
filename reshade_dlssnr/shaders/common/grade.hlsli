#pragma once

#include "bindings.hlsli"

static const float kGradeContrastPivot = 0.5;

bool GradeIsSdr()
{
    bool result = false;
    result = (int)(kGradeFormat + 0.5) == 0;
    return result;
}

float GradeLevelsFwd(float lightness, bool sdr)
{
    float result = lightness;
    const float black = clamp(kGradeBlackLevel, -0.25, 0.12);
    const float white = max(clamp(kGradeWhiteLevel, 0.90, 1.5), black + 0.05);
    result = (lightness - black) / (white - black);
    if (sdr)
        result = saturate(result);
    return result;
}

float GradeContrastFwd(float lightness, bool sdr)
{
    float result = lightness;
    if (kGradeContrast != 0.0)
    {
        if (!sdr)
        {
            result = (lightness - kGradeContrastPivot) * exp2(kGradeContrast * 0.5) +
                     kGradeContrastPivot;
        }
        else
        {
            const float t = saturate(lightness);
            const float exponent = exp2(kGradeContrast * 0.65);
            if (t < kGradeContrastPivot)
            {
                result = kGradeContrastPivot *
                         pow(max(t / kGradeContrastPivot, 0.0), exponent);
            }
            else
            {
                const float remaining =
                    (1.0 - t) / (1.0 - kGradeContrastPivot);
                result = 1.0 - (1.0 - kGradeContrastPivot) *
                         pow(max(remaining, 0.0), exponent);
            }
        }
    }
    return result;
}

float GradeShadowFwd(float lightness, float amount)
{
    float result = lightness;
    const float t = saturate(lightness);
    result = lightness + 0.24 * amount * t * (1.0 - t) * (1.0 - t);
    return result;
}

float GradeHighlightFwd(float lightness, float amount)
{
    float result = lightness;
    const float t = saturate(lightness);
    result = lightness + 0.24 * amount * t * t * (1.0 - t);
    return result;
}

float GradeSignedPow(float value, float exponent)
{
    float result = 0.0;
    if (value != 0.0)
        result = sign(value) * pow(abs(value), exponent);
    return result;
}

float GradeBrightnessFwd(float lightness, float amount, int mode)
{
    float result = lightness;
    if (amount != 0.0)
    {
        if (mode == 0)
            result = lightness * exp2(amount * 0.75);
        else if (mode == 1)
            result = GradeSignedPow(lightness, exp2(-amount * 0.7));
        else if (mode == 3)
        {
            const float t = saturate(lightness);
            result = lightness + 0.35 * amount * t * (1.0 - t);
        }
    }
    return result;
}

float3 LinearSrgbToOklab(float3 color)
{
    const float l = 0.4122214708 * color.r + 0.5363325363 * color.g +
                    0.0514459929 * color.b;
    const float m = 0.2119034982 * color.r + 0.6806995451 * color.g +
                    0.1073969566 * color.b;
    const float s = 0.0883024619 * color.r + 0.2817188376 * color.g +
                    0.6299787005 * color.b;

    const float l_ = sign(l) * pow(abs(l), 1.0 / 3.0);
    const float m_ = sign(m) * pow(abs(m), 1.0 / 3.0);
    const float s_ = sign(s) * pow(abs(s), 1.0 / 3.0);

    float3 result = float3(0.0, 0.0, 0.0);
    result = float3(
        0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
        1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
        0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_);
    return result;
}

float3 OklabToLinearSrgb(float3 lab)
{
    const float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    const float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    const float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    const float l = l_ * l_ * l_;
    const float m = m_ * m_ * m_;
    const float s = s_ * s_ * s_;

    float3 result = float3(0.0, 0.0, 0.0);
    result = float3(
         4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
    return result;
}

float3 Rec2020ToLinearSrgb(float3 color)
{
    float3 result = float3(0.0, 0.0, 0.0);
    result = float3(
         1.6604910021 * color.r - 0.5876411388 * color.g - 0.0728498633 * color.b,
        -0.1245504745 * color.r + 1.1328998971 * color.g - 0.0083494226 * color.b,
        -0.0181507634 * color.r - 0.1005788980 * color.g + 1.1187296614 * color.b);
    return result;
}

float3 LinearSrgbToRec2020(float3 color)
{
    float3 result = float3(0.0, 0.0, 0.0);
    result = float3(
        0.6274038959 * color.r + 0.3292830384 * color.g + 0.0433130657 * color.b,
        0.0690972894 * color.r + 0.9195403951 * color.g + 0.0113623195 * color.b,
        0.0163914389 * color.r + 0.0880133079 * color.g + 0.8955952532 * color.b);
    return result;
}

float3 GradeNativeToOklab(float3 color, bool rec2020)
{
    float3 result = float3(0.0, 0.0, 0.0);
    const float3 linear_srgb = rec2020 ? Rec2020ToLinearSrgb(color) : color;
    result = LinearSrgbToOklab(linear_srgb);
    return result;
}

float3 GradeOklabToNative(float3 lab, bool rec2020)
{
    float3 result = float3(0.0, 0.0, 0.0);
    const float3 linear_srgb = OklabToLinearSrgb(lab);
    result = rec2020 ? LinearSrgbToRec2020(linear_srgb) : linear_srgb;
    return result;
}

bool GradeRgbInGamut(float3 color, bool sdr)
{
    bool result = true;
    if (any(color < 0.0) || (sdr && any(color > 1.0)))
        result = false;
    return result;
}

float3 GradeGamutMap(float3 lab, bool sdr, bool rec2020)
{
    float3 result = float3(0.0, 0.0, 0.0);
    lab.x = sdr ? saturate(lab.x) : max(lab.x, 0.0);
    result = GradeOklabToNative(lab, rec2020);
    if (!GradeRgbInGamut(result, sdr))
    {
        float low = 0.0;
        float high = 1.0;
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            const float chroma_scale = (low + high) * 0.5;
            const float3 candidate = GradeOklabToNative(
                float3(lab.x, lab.y * chroma_scale, lab.z * chroma_scale), rec2020);
            if (GradeRgbInGamut(candidate, sdr))
                low = chroma_scale;
            else
                high = chroma_scale;
        }

        result = GradeOklabToNative(
            float3(lab.x, lab.y * low, lab.z * low), rec2020);
        result = sdr ? clamp(result, 0.0, 1.0) : max(result, 0.0);
    }
    return result;
}

float3 GradeLinearCoreFwd(float3 linear_rgb)
{
    float3 result = linear_rgb;
    const bool sdr = GradeIsSdr();
    const bool rec2020 = (int)(kGradeFormat + 0.5) == 2;
    const bool neutral = kGradeLuma == 0.0 && kGradeChroma == 0.0 &&
        kGradeBlackLevel == 0.0 && kGradeWhiteLevel == 1.0 &&
        kGradeContrast == 0.0 && kGradeShadows == 0.0 &&
        kGradeHighlights == 0.0;
    if (!neutral)
    {
        float3 lab = GradeNativeToOklab(linear_rgb, rec2020);
        lab.x = GradeLevelsFwd(lab.x, sdr);
        lab.x = GradeContrastFwd(lab.x, sdr);
        lab.x = GradeShadowFwd(lab.x, kGradeShadows);
        lab.x = GradeHighlightFwd(lab.x, kGradeHighlights);

        const int brightness_mode = (int)(kGradeLumaMode + 0.5);
        if (brightness_mode == 2)
        {
            lab *= exp2(kGradeLuma / 3.0);
        }
        else
        {
            lab.x = GradeBrightnessFwd(lab.x, kGradeLuma, brightness_mode);
        }

        if (kGradeChroma != 0.0)
        {
            float gain = max(1.0 + kGradeChroma, 0.05);
            if ((int)(kGradeChromaMode + 0.5) != 0)
            {
                const float chroma = length(lab.yz);
                const float normalized_chroma = chroma / max(abs(lab.x), 0.05);
                const float muted = 1.0 - saturate(normalized_chroma / 0.35);
                gain = max(1.0 + kGradeChroma * muted * muted, 0.05);
            }
            lab.yz = lab.yz * gain;
        }

        result = GradeGamutMap(lab, sdr, rec2020);
    }
    return result;
}

float3 GradeLinearFwd(float3 linear_rgb)
{
    float3 result = linear_rgb;
    if (kGradeEnabled >= 0.5)
        result = GradeLinearCoreFwd(linear_rgb);
    return result;
}

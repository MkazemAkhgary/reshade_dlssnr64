#ifndef DLSSNR_GRADE_HLSLI
#define DLSSNR_GRADE_HLSLI

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
            float lift = 0.5 * a;
            if (lift > 0.0)
                result = y + lift * (1.0 - y);
            else if (lift < 0.0)
                result = y + lift * y;
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
            float lift = 0.5 * a;
            if (lift > 0.0)
                result = (y - lift) / (1.0 - lift);
            else if (lift < 0.0)
                result = y / (1.0 + lift);
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

// Vibrance.
//
// Forward gain is:
//
//   g = 1 + a * (1 - 0.5*s)
//
// where s is the input RGB spread.
//
// The original inverse was wrong because it recomputed g from the already
// modified spread. Since outputSpread = g * inputSpread, we can recover the
// original gain exactly.
//
// For inputSpread < 1:
//   g^2 - (1+a)g + 0.5*a*outputSpread = 0
//
// For inputSpread >= 1:
//   g = 1 + 0.5*a
//

float GradeVibranceGain(float spread, float a)
{
    return 1.0 + a * (1.0 - 0.5 * saturate(spread));
}

float3 GradeChromaVibranceFwd(float3 c, float a)
{
    float y = dot(c, kL709);

    float spread =
        max(max(c.r, c.g), c.b) -
        min(min(c.r, c.g), c.b);

    float g = GradeVibranceGain(spread, a);

    return y + (c - y) * g;
}

float3 GradeChromaVibranceInv(float3 c, float a)
{
    float y = dot(c, kL709);

    float outputSpread =
        max(max(c.r, c.g), c.b) -
        min(min(c.r, c.g), c.b);

    float flatGain = 1.0 + 0.5 * a;

    float g;

    if (outputSpread >= flatGain)
    {
        // Corresponds to input spread >= 1.
        g = flatGain;
    }
    else
    {
        float disc =
            (1.0 + a) * (1.0 + a) -
            2.0 * a * outputSpread;

        disc = max(disc, 0.0);

        // The positive/larger root is the forward branch.
        g = 0.5 * ((1.0 + a) + sqrt(disc));
    }

    g = max(g, kGradeEps);

    return y + (c - y) / g;
}

// Chroma Gain.
//
// This uses a YCbCr-style decomposition whose coefficients are derived from
// the same kLuma709 weights used everywhere else. Therefore changing chroma
// cannot change luma except for normal floating-point rounding.
//
// Mathematically this is equivalent to uniformly scaling the chroma plane,
// which is the same operation as Saturation. Keeping the explicit YCbCr form
// here makes that property obvious and avoids the previous rounded-coefficient
// luma drift.

float3 GradeChromaGainFwd(float3 c, float a)
{
    float y = dot(c, kL709);
    float g = 1.0 + a;

    float cb = (c.b - y) / kCbScale;
    float cr = (c.r - y) / kCrScale;

    cb *= g;
    cr *= g;

    return float3(
        y + kCrScale * cr,
        y - kGCb * cb - kGCr * cr,
        y + kCbScale * cb
    );
}

float3 GradeChromaGainInv(float3 c, float a)
{
    float y = dot(c, kL709);
    float g = max(1.0 + a, kGradeEps);

    float cb = (c.b - y) / kCbScale;
    float cr = (c.r - y) / kCrScale;

    cb /= g;
    cr /= g;

    return float3(
        y + kCrScale * cr,
        y - kGCb * cb - kGCr * cr,
        y + kCbScale * cb
    );
}

float3 GradeChromaFwd(float3 c, float a, int mode)
{
    float3 result = c;
    if (a != 0.0)
    {
        if (mode == 0)
            result = GradeChromaSaturationFwd(c, a);
        else if (mode == 1)
            result = GradeChromaVibranceFwd(c, a);
        else
            result = GradeChromaGainFwd(c, a);
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
        else if (mode == 1)
            result = GradeChromaVibranceInv(c, a);
        else
            result = GradeChromaGainInv(c, a);
    }
    return result;
}

// -----------------------------------------------------------------------------
// Linear-domain grade core
// -----------------------------------------------------------------------------

float3 GradeLinearCoreFwd(float3 lin)
{
    float y0 = dot(lin, kL709);
    float y = y0;

    y = GradeLevelsFwd(y);
    y = GradeContrastFwd(y);
    y = GradeShadowFwd(y, kGradeShadows);
    y = GradeHighlightFwd(y, kGradeHighlights);
    y = GradeLumaFwd(
        y,
        kGradeLuma,
        (int)(kGradeLumaMode + 0.5)
    );

    // Change luma without dividing by y.
    // This is stable at/near black and preserves the chroma residual.
    float3 c = lin + (y - y0);

    return GradeChromaFwd(
        c,
        kGradeChroma,
        (int)(kGradeChromaMode + 0.5)
    );
}

float3 GradeLinearCoreInv(float3 lin)
{
    // Undo chroma first because chroma preserves luma.
    float3 c = GradeChromaInv(
        lin,
        kGradeChroma,
        (int)(kGradeChromaMode + 0.5)
    );

    float y = dot(c, kL709);

    y = GradeLumaInv(
        y,
        kGradeLuma,
        (int)(kGradeLumaMode + 0.5)
    );

    y = GradeHighlightInv(y, kGradeHighlights);
    y = GradeShadowInv(y, kGradeShadows);
    y = GradeContrastInv(y);
    y = GradeLevelsInv(y);

    float currentY = dot(c, kL709);

    // Restore the original luma without a division by currentY.
    return c + (y - currentY);
}

// -----------------------------------------------------------------------------
// sRGB-domain wrappers.
//
// These retain the old names for callers that actually have sRGB values,
// but the grade itself is still performed in linear light.
// -----------------------------------------------------------------------------

float3 GradeSdrFwd(float3 sdr)
{
    return LinearToSrgbExt(
        GradeLinearCoreFwd(
            SrgbToLinearExt(sdr)
        )
    );
}

float3 GradeSdrInv(float3 sdr)
{
    return LinearToSrgbExt(
        GradeLinearCoreInv(
            SrgbToLinearExt(sdr)
        )
    );
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

#endif

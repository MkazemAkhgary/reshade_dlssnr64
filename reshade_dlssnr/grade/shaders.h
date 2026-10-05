// Grade shaders. Applies user-selected levels, contrast, region weights,
// luma and chroma operators. Used at three points: model input, model
// output, and the backbuffer.
#pragma once

namespace reshade_dlssnr {

// Prepend to any shader that calls GradeLinearFwd / GradeLinearInv. Declares
// the grade cbuffer, the uniform layout, and the operator chain. ApplyGrade
// reads the same layout.
    inline constexpr const char* kGradePreludeSource = R"(
cbuffer Params : register(b0)
{
    uint  gWidth;
    uint  gHeight;
    float4 gParams[13];
};

#define kGradeEnabled    gParams[2].x
#define kGradeLuma       gParams[2].y
#define kGradeChroma     gParams[2].z
#define kGradeLumaMode   gParams[2].w
#define kGradeChromaMode gParams[3].x
#define kGradeBlackLevel gParams[3].y
#define kGradeWhiteLevel gParams[3].z
#define kGradeContrast   gParams[3].w
#define kGradeShadows    gParams[4].x
#define kGradeHighlights gParams[4].y
#define kGradeFormat     gParams[4].z

static const float3 kL709 = float3(0.2126, 0.7152, 0.0722);

// Exact BT.709 YCbCr-style scale factors derived from the luma weights.
// Using the ratios below rather than rounded reconstruction coefficients
// keeps chroma gain exactly luma-preserving (to floating-point precision).
static const float kCbScale = 2.0 * (1.0 - kL709.z); // 1.8556
static const float kCrScale = 2.0 * (1.0 - kL709.x); // 1.5748
static const float kGCb = (kL709.z * kCbScale) / kL709.y;
static const float kGCr = (kL709.x * kCrScale) / kL709.y;

static const float kGradeEps = 1e-6;

// -----------------------------------------------------------------------------
// Extended sRGB transfer.
//
// The standard sRGB formulas are defined for non-negative normalized values.
// These versions use a signed extension for floating-point/HDR intermediates,
// which avoids silently destroying negative values created by an intermediate
// grade operation.
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// Levels
// -----------------------------------------------------------------------------

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
    if (kGradeContrast == 0.0)
        return y;

    return (y - 0.5) * exp2(kGradeContrast) + 0.5;
}

float GradeContrastInv(float y)
{
    if (kGradeContrast == 0.0)
        return y;

    return (y - 0.5) / exp2(kGradeContrast) + 0.5;
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
    if (a == 0.0)
        return y;

    float t = saturate(y);
    float room = 1.0 - t;

    return y + 0.5 * a * y * room * room;
}

float GradeShadowInv(float y_out, float a)
{
    if (a == 0.0)
        return y_out;

    // Below black, the mapping is simply y*(1 + 0.5*a).
    if (y_out <= 0.0)
        return y_out / (1.0 + 0.5 * a);

    // Above white, shadow shaping is disabled.
    if (y_out >= 1.0)
        return y_out;

    // Exact inverse of the cubic on [0,1].
    float y = saturate(y_out);

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float room = 1.0 - y;
        float f = y + 0.5 * a * y * room * room - y_out;

        float df = 1.0 + 0.5 * a *
                   (room * room - 2.0 * y * room);

        y -= f / max(abs(df), kGradeEps) *
             sign(df);

        y = saturate(y);
    }

    return y;
}

float GradeHighlightFwd(float y, float a)
{
    if (a == 0.0)
        return y;

    float t = saturate(y);

    return y + 0.5 * a * t * t * (1.0 - y);
}

float GradeHighlightInv(float y_out, float a)
{
    if (a == 0.0)
        return y_out;

    // Below black, highlight shaping is disabled.
    if (y_out <= 0.0)
        return y_out;

    // Above white, the mapping is linear:
    // y_out = y + 0.5*a*(1-y)
    if (y_out >= 1.0)
        return (y_out - 0.5 * a) / (1.0 - 0.5 * a);

    // Exact inverse of the cubic on [0,1].
    float y = saturate(y_out);

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float f = y + 0.5 * a * y * y * (1.0 - y) - y_out;

        float df = 1.0 + 0.5 * a *
                   (2.0 * y * (1.0 - y) - y * y);

        y -= f / max(abs(df), kGradeEps) *
             sign(df);

        y = saturate(y);
    }

    return y;
}

// -----------------------------------------------------------------------------
// Luma modes
// -----------------------------------------------------------------------------

float SignedPow(float y, float p)
{
    if (y == 0.0)
        return 0.0;

    return (y < 0.0 ? -1.0 : 1.0) *
           pow(abs(y), p);
}

float GradeLumaFwd(float y, float a, int mode)
{
    if (a == 0.0)
        return y;

    if (mode == 0)
    {
        // Gain
        return y * exp2(a);
    }

    if (mode == 1)
    {
        // Gamma. Signed extension keeps HDR/negative intermediates invertible.
        return SignedPow(y, exp2(-a));
    }

    if (mode == 2)
    {
        // Lift. Exact inverse for the full [-1,1] UI range.
        float lift = 0.5 * a;

        if (lift > 0.0)
            return y + lift * (1.0 - y);

        if (lift < 0.0)
            return y + lift * y;

        return y;
    }

    // Soft:
    // zero at both ends, strongest at mid-grey.
    //
    // saturate(y) makes the effect disappear outside [0,1], avoiding
    // surprising HDR amplification.
    float t = saturate(y);
    return y + 0.5 * a * t * (1.0 - t);
}

float GradeLumaInv(float y, float a, int mode)
{
    if (a == 0.0)
        return y;

    if (mode == 0)
    {
        return y / exp2(a);
    }

    if (mode == 1)
    {
        return SignedPow(y, exp2(a));
    }

    if (mode == 2)
    {
        float lift = 0.5 * a;

        if (lift > 0.0)
            return (y - lift) / (1.0 - lift);

        if (lift < 0.0)
            return y / (1.0 + lift);

        return y;
    }

    // Soft inverse.
    if (y <= 0.0 || y >= 1.0)
        return y;

    float x = saturate(y);

    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        float f = x + 0.5 * a * x * (1.0 - x) - y;
        float df = 1.0 + 0.5 * a * (1.0 - 2.0 * x);

        x -= f / max(abs(df), kGradeEps) *
             sign(df);

        x = saturate(x);
    }

    return x;
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
// the same kL709 weights used everywhere else. Therefore changing chroma
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
    if (a == 0.0)
        return c;

    if (mode == 0)
        return GradeChromaSaturationFwd(c, a);

    if (mode == 1)
        return GradeChromaVibranceFwd(c, a);

    return GradeChromaGainFwd(c, a);
}

float3 GradeChromaInv(float3 c, float a, int mode)
{
    if (a == 0.0)
        return c;

    if (mode == 0)
        return GradeChromaSaturationInv(c, a);

    if (mode == 1)
        return GradeChromaVibranceInv(c, a);

    return GradeChromaGainInv(c, a);
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
    if (kGradeEnabled < 0.5)
        return lin;

    return GradeLinearCoreFwd(lin);
}

float3 GradeLinearInv(float3 lin)
{
    if (kGradeEnabled < 0.5)
        return lin;

    return GradeLinearCoreInv(lin);
}
)";

// Standalone pass: decodes the target from its own format, applies the grade,
// re-encodes.
    inline constexpr const char* kApplyGradeSource = R"(
Texture2D<float4> gSource : register(t0);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

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

    return 10000.0 *
           pow(num / max(den, 1e-6), 1.0 / m1);
}

float3 NitsToPq(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 Y = max(nits / 10000.0, 0.0);
    float3 Ym1 = pow(Y, m1);

    float3 num = c1 + c2 * Ym1;
    float3 den = 1.0 + c3 * Ym1;

    return pow(num / den, m2);
}

float3 DecodeSource(float3 c)
{
    int fmt = (int)(kGradeFormat + 0.5);

    if (fmt == 1)
        return max(c, 0.0) * (80.0 / 100.0);

    if (fmt == 2)
        return PqToNits(c) / 100.0;

    return SrgbToLinearExt(c);
}

float3 EncodeSource(float3 lin)
{
    int fmt = (int)(kGradeFormat + 0.5);

    if (fmt == 1)
        return max(lin, 0.0) * (100.0 / 80.0);

    if (fmt == 2)
        return NitsToPq(max(lin, 0.0) * 100.0);

    // Standard SDR sRGB encoding.
    //
    // Values below zero cannot be represented in SDR.
    // Values above one cannot be represented either, so the final saturate()
    // is the correct gamut/range operation rather than a creative shoulder.
    return saturate(LinearToSrgbExt(max(lin, 0.0)));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv =
        (float2(id.xy) + 0.5) /
        float2(gWidth, gHeight);

    float4 src =
        gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeSource(src.rgb);
    float3 graded = GradeLinearFwd(lin);

    gOutput[id.xy] =
        float4(EncodeSource(graded), src.a);
}
)";

}  // namespace reshade_dlssnr

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
#define kGradeBlackPoint gParams[3].y
#define kGradeWhitePoint gParams[3].z
#define kGradeContrast   gParams[3].w
#define kGradeShadows    gParams[4].x
#define kGradeHighlights gParams[4].y
#define kGradeFormat     gParams[4].z

static const float3 kL709 = float3(0.2126, 0.7152, 0.0722);

float3 SrgbToLinearExt(float3 v) {
    v = max(v, 0.0);
    float3 lo = v / 12.92;
    float3 hi = pow((v + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, v));
}
float3 LinearToSrgbExt(float3 v) {
    v = max(v, 0.0);
    float3 lo = v * 12.92;
    float3 hi = 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, v));
}

float GradeLevelsFwd(float y) {
    float span = max(kGradeWhitePoint - kGradeBlackPoint, 0.05);
    return (y - kGradeBlackPoint) / span;
}
float GradeLevelsInv(float y) {
    float span = max(kGradeWhitePoint - kGradeBlackPoint, 0.05);
    return y * span + kGradeBlackPoint;
}

float GradeContrastFwd(float y) {
    if (kGradeContrast == 0.0) return y;
    return (y - 0.5) * exp2(kGradeContrast) + 0.5;
}
float GradeContrastInv(float y) {
    if (kGradeContrast == 0.0) return y;
    return (y - 0.5) / exp2(kGradeContrast) + 0.5;
}

float ShadowWeight(float y) {
    float t = saturate(y);
    return exp(-t * t * 6.0);
}
float HighlightWeight(float y) {
    float t = 1.0 - saturate(y);
    return exp(-t * t * 6.0);
}

float GradeShadowFwd(float y, float a) {
    if (a == 0.0) return y;
    float w = ShadowWeight(y);
    float room = a > 0.0 ? max(1.0 - y, 0.0) : max(y, 0.0);
    return y + a * 0.5 * w * room;
}
float GradeHighlightFwd(float y, float a) {
    if (a == 0.0) return y;
    float w = HighlightWeight(y);
    float room = a > 0.0 ? max(1.0 - y, 0.0) : max(y, 0.0);
    return y + a * 0.5 * w * room;
}
float GradeShadowInv(float y_out, float a) {
    if (a == 0.0) return y_out;
    float y = y_out;
    for (int i = 0; i < 4; ++i) {
        float t = saturate(y);
        float w = exp(-t * t * 6.0);
        float room = a > 0.0 ? max(1.0 - y, 0.0) : max(y, 0.0);
        float droom = a > 0.0 ? -1.0 : 1.0;
        float dw = (y > 0.0 && y < 1.0) ? -12.0 * y * w : 0.0;
        float f = y + a * 0.5 * w * room - y_out;
        float df = 1.0 + a * 0.5 * (dw * room + w * droom);
        if (abs(df) < 1e-6) break;
        y -= f / df;
    }
    return y;
}
float GradeHighlightInv(float y_out, float a) {
    if (a == 0.0) return y_out;
    float y = y_out;
    for (int i = 0; i < 4; ++i) {
        float t = 1.0 - saturate(y);
        float w = exp(-t * t * 6.0);
        float room = a > 0.0 ? max(1.0 - y, 0.0) : max(y, 0.0);
        float droom = a > 0.0 ? -1.0 : 1.0;
        float dw = (y > 0.0 && y < 1.0) ? 12.0 * (1.0 - y) * w : 0.0;
        float f = y + a * 0.5 * w * room - y_out;
        float df = 1.0 + a * 0.5 * (dw * room + w * droom);
        if (abs(df) < 1e-6) break;
        y -= f / df;
    }
    return y;
}

float GradeLumaFwd(float y, float a, int mode) {
    if (a == 0.0) return y;
    if (mode == 0) return y * exp2(a);
    if (mode == 1) return pow(max(y, 1e-6), exp2(-a));
    if (mode == 2) {
        float lift = a * 0.5;
        float room = lift > 0.0 ? max(1.0 - y, 0.0) : max(y, 0.0);
        return y + lift * room;
    }
    return y + a * y * max(1.0 - y, 0.0);
}
float GradeLumaInv(float y, float a, int mode) {
    if (a == 0.0) return y;
    if (mode == 0) return y / exp2(a);
    if (mode == 1) return pow(max(y, 1e-6), exp2(a));
    if (mode == 2) {
        float lift = a * 0.5;
        if (lift > 0.0) return (y - lift) / max(1.0 - lift, 1e-3);
        return y / max(1.0 + lift, 1e-3);
    }
    float A = -a;
    if (abs(A) < 1e-6) return y;
    float B = 1.0 + a;
    float C = -y;
    float disc = B * B - 4.0 * A * C;
    if (disc < 0.0) return y;
    float sq = sqrt(disc);
    float r1 = (-B - sq) / (2.0 * A);
    float r2 = (-B + sq) / (2.0 * A);
    if (r1 >= 0.0 && r1 <= 1.0) return r1;
    if (r2 >= 0.0 && r2 <= 1.0) return r2;
    return (a > 0.0) ? r1 : r2;
}

float3 GradeChromaFwd(float3 c, float a, int mode) {
    if (a == 0.0) return c;
    float y = dot(c, kL709);
    if (mode == 0) return lerp(float3(y, y, y), c, 1.0 + a);
    if (mode == 1) {
        float spread = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
        float k = 1.0 + a * (1.0 - 0.5 * saturate(spread));
        return lerp(float3(y, y, y), c, k);
    }
    float g = 1.0 + a;
    float cb = (c.b - y) / 1.8556 * g;
    float cr = (c.r - y) / 1.5748 * g;
    return float3(y + 1.5748 * cr,
                  y - 0.1873 * cb - 0.4681 * cr,
                  y + 1.8556 * cb);
}
float3 GradeChromaInv(float3 c, float a, int mode) {
    if (a == 0.0) return c;
    float y = dot(c, kL709);
    if (mode == 0) return lerp(float3(y, y, y), c, 1.0 / max(1.0 + a, 1e-3));
    if (mode == 1) {
        float spread = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
        float k = 1.0 + a * (1.0 - 0.5 * saturate(spread));
        return lerp(float3(y, y, y), c, 1.0 / max(k, 1e-3));
    }
    float g = 1.0 + a;
    float cb = (c.b - y) / 1.8556 / max(g, 1e-3);
    float cr = (c.r - y) / 1.5748 / max(g, 1e-3);
    return float3(y + 1.5748 * cr,
                  y - 0.1873 * cb - 0.4681 * cr,
                  y + 1.8556 * cb);
}

float3 GradeSdrFwd(float3 sdr) {
    float y = dot(sdr, kL709);
    float ny = GradeLevelsFwd(y);
    ny = GradeContrastFwd(ny);
    ny = GradeShadowFwd(ny, kGradeShadows);
    ny = GradeHighlightFwd(ny, kGradeHighlights);
    ny = GradeLumaFwd(ny, kGradeLuma, (int)(kGradeLumaMode + 0.5));
    float sy = (y != 0.0) ? y : 1.0;
    float3 c = sdr * (ny / sy);
    return GradeChromaFwd(c, kGradeChroma, (int)(kGradeChromaMode + 0.5));
}
float3 GradeSdrInv(float3 sdr) {
    float3 c = GradeChromaInv(sdr, kGradeChroma, (int)(kGradeChromaMode + 0.5));
    float ny = dot(c, kL709);
    float y = ny;
    y = GradeLumaInv(y, kGradeLuma, (int)(kGradeLumaMode + 0.5));
    y = GradeHighlightInv(y, kGradeHighlights);
    y = GradeShadowInv(y, kGradeShadows);
    y = GradeContrastInv(y);
    y = GradeLevelsInv(y);
    float sny = (ny != 0.0) ? ny : 1.0;
    return c * (y / sny);
}

float3 GradeLinearFwd(float3 lin) {
    if (kGradeEnabled < 0.5) return lin;
    return SrgbToLinearExt(GradeSdrFwd(LinearToSrgbExt(lin)));
}
float3 GradeLinearInv(float3 lin) {
    if (kGradeEnabled < 0.5) return lin;
    return SrgbToLinearExt(GradeSdrInv(LinearToSrgbExt(lin)));
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
    return 10000.0 * pow(num / max(den, 1e-6), 1.0 / m1);
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
    if (fmt == 1) return max(c, 0.0) * (80.0 / 100.0);
    if (fmt == 2) return max(PqToNits(c), 0.0) / 100.0;
    return SrgbToLinearExt(c);
}

float3 EncodeSource(float3 lin)
{
    int fmt = (int)(kGradeFormat + 0.5);
    if (fmt == 1) return max(lin, 0.0) * (100.0 / 80.0);
    if (fmt == 2) return NitsToPq(max(lin, 0.0) * 100.0);
    float3 sdr = LinearToSrgbExt(lin);
    const float knee = 0.9;
    const float room = 1.0 - knee;
    float3 over = max(sdr - knee, 0.0);
    return min(sdr, knee) + room * over / (over + room);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(gWidth, gHeight);
    float4 src = gSource.SampleLevel(s0, uv, 0);

    float3 lin = DecodeSource(src.rgb);
    float3 graded = GradeLinearFwd(lin);
    gOutput[id.xy] = float4(EncodeSource(graded), src.a);
}
)";

}  // namespace reshade_dlssnr

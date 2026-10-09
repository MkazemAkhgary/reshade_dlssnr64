#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4> gComposed : register(t0);
Texture2D<float4> gSource   : register(t1);
Texture2D<float4> gModelOut : register(t2);
Texture2D<float>  gEffectiveExposure : register(t3);
RWTexture2D<float4> gOutput : register(u0);
SamplerState s0 : register(s0);

#define gLowInput gSource
#define gLowEnhanced gModelOut
#include "common/fsr.hlsli"
#undef gLowInput
#undef gLowEnhanced

#define kDiagMode shader.Params0.x
#define kEffectiveExposureMapValid shader.Params1.w
#define kLocalExposureStrength hdr.LocalExposure.x

float3 ForwardHdrToModelSdr(float3 hdr_encoded, uint2 pixel)
{
    float3 hdr_linear = DecodeToLinear(
        hdr_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
    float3 pre_hdr = HdrPreDesaturate(hdr_linear, saturate(kCrosstalk));

    float3 pre_exposed = pre_hdr * max(kExposure, 1e-6);
    float y_exposed = max(dot(pre_exposed, HdrLumaWeights()), 0.0);

    float effective_peak_nits = max(
        kSdrReferenceWhiteNits, max(kEffectiveSceneWhiteNits, 0.01));
    float y_curve = y_exposed * kPqPeakNits / effective_peak_nits;
    float y_sdr = max(HdrBt2446Forward(y_curve) / 100.0, 0.0);
    float chroma_scale = y_exposed > 1e-8 ? y_sdr / y_exposed : 0.0;
    float3 pre_sdr = pre_exposed * chroma_scale;
    float3 sdr_global = HdrInversePreDesaturate(
        pre_sdr, saturate(kCrosstalk));

    float local_scale = 1.0;
    if (kLocalExposureStrength > 0.0 && kEffectiveExposureMapValid > 0.5)
        local_scale = max(gEffectiveExposure.Load(int3(pixel.xy, 0)), 1e-6);

    return LinearToSrgb(
        GamutFitModelInput(max(sdr_global * local_scale, 0.0)));
}

float3 EncodeDiagnosticSdr(float3 sdr)
{
    int fmt = (int)(kFormat + 0.5);
    float3 v = max(sdr, 0.0);
    if (fmt == 1)
        return v * (kSdrReferenceWhiteNits / kScRgbReferenceWhiteNits);
    if (fmt == 2)
        return NitsToPq(v * kSdrReferenceWhiteNits);
    return v;
}

// Compare the composed HDR against an upsampled model endpoint using the same
// local-exposure map and RCAS settings as composition. At reduced working scales,
// retained full-resolution source detail can still differ from the upsampled view.
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);
    float3 roundtrip = ForwardHdrToModelSdr(
        gComposed.SampleLevel(s0, uv, 0.0).rgb, id.xy);
    const float2 easu_con3 = float2(0.0, 4.0 * kEasuCon1.y);
    float3 model_in = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                               easu_con3, 0);
    float3 model_out = FsrEasuF(id.xy, kEasuCon0, kEasuCon1, kEasuCon2,
                                easu_con3, 1);

    if (kRcasEnabled > 0.5)
    {
        const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);

        float3 b_in = gSource.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_in = gSource.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_in = gSource.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_in = gSource.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        model_in = FsrRcasF(b_in, d_in, model_in, f_in, h_in, kRcasStrength);

        float3 b_out = gModelOut.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_out = gModelOut.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_out = gModelOut.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_out = gModelOut.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        model_out = FsrRcasF(b_out, d_out, model_out, f_out, h_out, kRcasStrength);
    }

    int mode = (int)(kDiagMode + 0.5);
    float3 display = 0.0;
    if (mode == 0) {
        bool right = uv.x >= 0.5;
        bool bottom = uv.y >= 0.5;
        if (!bottom && !right)
            display = model_in;
        else if (!bottom && right)
            display = model_out;
        else if (bottom && !right)
            display = roundtrip;
        else {
            float3 diff = roundtrip - model_out;
            display = saturate(0.5 + diff * 4.0);
        }
    } else if (mode == 1) {
        float3 diff = roundtrip - model_out;
        display = saturate(0.5 + diff * 4.0);
    } else if (mode == 2) {
        float local_scale = kLocalExposureStrength > 0.0 &&
                kEffectiveExposureMapValid > 0.5
            ? max(gEffectiveExposure.Load(int3(id.xy, 0)), 1e-6)
            : 1.0;
        float stops = log2(local_scale);
        display = saturate(0.5 + stops / 4.0).xxx;
    }

    gOutput[id.xy] = float4(EncodeDiagnosticSdr(display), 1.0);
}

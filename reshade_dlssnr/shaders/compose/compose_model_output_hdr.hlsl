#include "common/cbuffer.hlsli"
#include "common/bindings.hlsli"
#include "common/registers.hlsli"
#include "common/color.hlsli"
#include "common/luma.hlsli"
#include "common/math.hlsli"
#include "common/grade.hlsli"
#include "common/compose_common.hlsli"
#include "common/hdr_transfer.hlsli"

Texture2D<float4>   gOriginal    : register(t0);
Texture2D<float4>   gLowInput    : register(t1);
Texture2D<float4>   gLowEnhanced : register(t2);
Texture2D<float4>   gHdrSource   : register(t3);
RWTexture2D<float4> gOutput      : register(u0);
SamplerState        s0           : register(s0);

#define kLocalExposureStrength hdr.LocalExposure.x

#include "common/fsr.hlsli"

// ---------------------------------------------------------------------------
// Joint bilateral upsampling, guided by a full-resolution image.
//
// Each full-resolution output pixel is the weighted average of the 3x3
// low-resolution samples nearest its projected low-res coordinate. The
// weight of a low-res sample is exp(-|dG| * sharpness), where dG is the
// difference between the guide luma at that sample's corresponding
// full-resolution position and the guide luma at the output pixel.
//
// At working scale 1.0 the caller reads the low-res texture directly, so
// this helper is only reached when the projection spans multiple source
// pixels.
// ---------------------------------------------------------------------------
float3 JointBilateralUpsample(
    int2 full_xy,
    int2 full_size,
    Texture2D<float4> low_tex,
    Texture2D<float4> guide_tex,
    float sharpness)
{
    uint low_w_u = 0u;
    uint low_h_u = 0u;
    low_tex.GetDimensions(low_w_u, low_h_u);
    const int low_w = max((int)low_w_u, 1);
    const int low_h = max((int)low_h_u, 1);
    if (full_size.x <= 0 || full_size.y <= 0) return float3(0.0, 0.0, 0.0);

    const float2 full_sz = float2((float)full_size.x, (float)full_size.y);
    const float2 low_sz  = float2((float)low_w, (float)low_h);

    // Pixel-center projection from full-res to low-res space. At 1:1 this
    // lands exactly on the source pixel; at other scales it interpolates.
    const float2 full_center = float2((float)full_xy.x + 0.5,
                                      (float)full_xy.y + 0.5);
    const float2 low_pix = full_center * (low_sz / full_sz);
    const int2 low_center = int2((int)floor(low_pix.x),
                                 (int)floor(low_pix.y));

    const float3 guide_center = guide_tex.Load(int3(full_xy, 0)).rgb;
    const float guide_luma_center =
        dot(guide_center, float3(0.2126, 0.7152, 0.0722));

    float3 accum = float3(0.0, 0.0, 0.0);
    float  weight_sum = 0.0;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 low_xy = int2(low_center.x + dx, low_center.y + dy);
            if (low_xy.x < 0 || low_xy.y < 0 ||
                low_xy.x >= low_w || low_xy.y >= low_h) {
                continue;
            }

            // Full-res position of this low-res sample's center, so the
            // guide is read at the same feature the sample represents.
            const float2 low_neighbor_uv = float2(
                ((float)low_xy.x + 0.5) / (float)low_w,
                ((float)low_xy.y + 0.5) / (float)low_h);
            int2 full_neighbor_xy = int2(
                (int)floor(low_neighbor_uv.x * full_sz.x),
                (int)floor(low_neighbor_uv.y * full_sz.y));
            full_neighbor_xy.x =
                clamp(full_neighbor_xy.x, 0, full_size.x - 1);
            full_neighbor_xy.y =
                clamp(full_neighbor_xy.y, 0, full_size.y - 1);

            const float3 guide_neighbor =
                guide_tex.Load(int3(full_neighbor_xy, 0)).rgb;
            const float guide_luma_neighbor =
                dot(guide_neighbor, float3(0.2126, 0.7152, 0.0722));

            const float luma_diff =
                abs(guide_luma_neighbor - guide_luma_center);
            const float weight = exp(-luma_diff * sharpness);

            const float3 low_val = low_tex.Load(int3(low_xy, 0)).rgb;
            accum += low_val * weight;
            weight_sum += weight;
        }
    }

    if (weight_sum > 0.0) return accum / weight_sum;

    // Degenerate weight sum: fall back to the projected sample directly.
    const int2 clamped_center = int2(
        clamp(low_center.x, 0, low_w - 1),
        clamp(low_center.y, 0, low_h - 1));
    return low_tex.Load(int3(clamped_center, 0)).rgb;
}


float3 DecodePackedHdr(float4 packed)
{
    return packed.rgb;
}


float3 EncodeHdr(float3 lin)
{
    return HdrEncodeTarget(lin, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);
}

// Gain-Map Residual measures the model edit as a relative change in the
// actual linear model domain. Do not invert the nonlinear gamut fit on both
// endpoints: near white, tiny model changes would become enormous HDR deltas.
// The stored reciprocal multiplier transfers the relative edit back onto the
// original HDR source without altering its exposure or highlight range.
float3 ComposeHdrInverse(float3 sdr_base, float3 hdr_src,
                         float3 low_in, float3 low_out,
                         float carrier_to_hdr_scale)
{
    float3 result = hdr_src;
    float3 low_in_lin = max(SrgbToLinearExt(low_in), 0.0);
    float3 low_out_lin = max(SrgbToLinearExt(low_out), 0.0);
    if (kGradeEnabled > 0.5)
    {
        low_in_lin = max(GradeLinearInv(low_in_lin), 0.0);
        low_out_lin = max(GradeLinearInv(low_out_lin), 0.0);
    }

    const float3 model_delta = low_out_lin - low_in_lin;
    if (!all(model_delta == 0.0))
    {
        const float3 model_denominator = max(abs(low_in_lin), 0.001.xxx);
        const float3 relative_delta = model_delta / model_denominator;
        const float3 model_sdr_base = GamutFitModelInput(max(sdr_base, 0.0));
        const float3 kL = HdrLumaWeights();
        float base_luma = dot(model_sdr_base, kL);
        if (kLocalExposureStrength > 0.0)
            base_luma = max(dot(low_in_lin, kL), 0.0);
        const float3 shaped = ShapeResidual(relative_delta, base_luma);

        // packed.rgb * packed.a reconstructs this pixel's scene-referred source
        // scale. Applying the relative edit in this domain preserves super-whites;
        // no inverse gamut curve, hard highlight ceiling, or tone-curve re-evaluation
        // is needed during composition.
        const float3 carrier_delta = max(sdr_base, 0.0) * shaped;
        const float3 hdr_edited = max(
            hdr_src + carrier_delta * max(carrier_to_hdr_scale, 0.0), 0.0);
        result = BlendYCbCr(hdr_src, hdr_edited, kLumaStrength, kChromaStrength);
    }
    return result;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(Width, Height);

    float4 packed = gOriginal.Load(int3(id.xy, 0));
    const float3 source_encoded = gHdrSource.Load(int3(id.xy, 0)).rgb;
    const float3 hdr_src = DecodeToLinear(
        source_encoded, (int)(kFormat + 0.5), kSdrReferenceWhiteNits);

    // EASU upsample from the actual model working resolution back to full HDR
    // resolution. Both textures are in the same sRGB-encoded model domain.
    float3 low_in;
    float3 low_out;
    if (kEasuCon0.x == 1.0 && kEasuCon0.y == 1.0) {
        // With matching working/output extents, reconstruction is identity.
        low_in = gLowInput.Load(int3(id.xy, 0)).rgb;
        low_out = gLowEnhanced.Load(int3(id.xy, 0)).rgb;
    } else {
        // Joint bilateral upsample the model input and output separately,
        // guided by the full-resolution packed SDR carrier. The carrier's RGB
        // is the linear SDR base the model was trained on, so its edges line
        // up with the model-domain reconstruction. Replaces EASU, which
        // degrades at low working scales. The residual math in
        // ComposeHdrInverse is unchanged and still receives low_in / low_out.
        const int2 full_size_int = int2((int)Width, (int)Height);
        const int2 full_xy = int2((int)id.x, (int)id.y);
        low_in  = JointBilateralUpsample(
            full_xy, full_size_int, gLowInput,  gOriginal, 8.0);
        low_out = JointBilateralUpsample(
            full_xy, full_size_int, gLowEnhanced, gOriginal, 8.0);
    }

    if (kRcasEnabled > 0.5) {
        const float2 texel = float2(kEasuCon1.x, kEasuCon1.y);

        float3 b_in = gLowInput.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_in = gLowInput.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_in = gLowInput.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_in = gLowInput.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_in = FsrRcasF(b_in, d_in, low_in, f_in, h_in, kRcasStrength);

        float3 b_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, -texel.y), 0).rgb;
        float3 d_out = gLowEnhanced.SampleLevel(s0, uv + float2(-texel.x, 0.0), 0).rgb;
        float3 f_out = gLowEnhanced.SampleLevel(s0, uv + float2(texel.x, 0.0), 0).rgb;
        float3 h_out = gLowEnhanced.SampleLevel(s0, uv + float2(0.0, texel.y), 0).rgb;
        low_out = FsrRcasF(b_out, d_out, low_out, f_out, h_out, kRcasStrength);
    }

    const float3 sdr_base = DecodePackedHdr(packed);
    const float carrier_to_hdr_scale = max(packed.a, 0.0);
    const float3 final_lin = ComposeHdrInverse(
        sdr_base, hdr_src, low_in, low_out, carrier_to_hdr_scale);
    const float3 encoded_result = EncodeHdr(final_lin);
    // Preserve native-format codes in untouched channels rather than running
    // their source values through a lossy decode/encode pair.
    const float3 final_out = float3(
        final_lin.r == hdr_src.r ? source_encoded.r : encoded_result.r,
        final_lin.g == hdr_src.g ? source_encoded.g : encoded_result.g,
        final_lin.b == hdr_src.b ? source_encoded.b : encoded_result.b);
    gOutput[id.xy] = float4(final_out, 1.0);
}

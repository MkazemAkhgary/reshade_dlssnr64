#pragma once

// One file shared by MSVC and D3DCompile's HLSL preprocessor.
//
// Layout rules for the payload blocks:
//   1. Use float, float4, and structs from this file; no float2/float3 fields.
//   2. Every block is a multiple of 16 bytes. Pad explicitly.
//   3. No packoffset and no platform headers/intrinsics.
//   4. C++ blocks are alignas(16); the outer ComputeConstants is alignas(256).
//   5. gpu/compute_constants.h asserts size and offsets.

#ifdef __cplusplus
  struct alignas(16) float4 { float x, y, z, w; };
  using DLSSNR_UINT = unsigned int;
  #define DLSSNR_BLOCK(name)   struct alignas(16) name
  #define DLSSNR_CBUFFER(name) struct alignas(256) name
#else
  #define DLSSNR_UINT uint
  #define DLSSNR_BLOCK(name)   struct name
  #define DLSSNR_CBUFFER(name) cbuffer name : register(b0)
#endif

DLSSNR_BLOCK(ShaderBlock)
{
    float4 Params0;   // LumaStrength, ChromaStrength, SoftCompression, SoftMaxBrighten
    float4 Params1;   // SoftMaxDarken, SoftMaxChroma, SoftKnee, UpsampleKernelHalf
};

DLSSNR_BLOCK(GradeBlock)
{
    float Enabled;
    float Luma;
    float Chroma;
    float LumaMode;
    float ChromaMode;
    float BlackLevel;
    float WhiteLevel;
    float Contrast;
    float Shadows;
    float Highlights;
    float Format;
    float RestoreExposure;
};

DLSSNR_BLOCK(ResidualBlock)
{
    float EditStrength;
    float ChromaStrength;
    float EditGamma;
    float HighlightProtect;
    float ShadowProtect;
    float RatioEps;
    float RatioBrighten;
    float RatioDarken;
};

DLSSNR_BLOCK(HdrBlock)
{
    float Format;
    float Exposure;
    float Bt2446K1;
    float Bt2446K3;
    float Bt2446Ip;
    float Crosstalk;
    // Compose upsample selection.
    //   UpsampleMode: 0 = FSR 1.0 EASU, 1 = joint bilateral.
    //   UpsampleSharpness: guide-difference exponent for the bilateral
    //     path; ignored when mode selects EASU.
    float UpsampleMode;
    float UpsampleSharpness;
    float GainMinLog2;
    float GainMaxLog2;
    // Absolute effective scene-white peak in nits.
    float EffectiveSceneWhiteNits;
    // x = local exposure strength, y = max brighten stops, z = max darken
    // stops, w = model midtone target. Zero strength disables local exposure.
    float4 LocalExposure;
    // x = neighborhood radius in source pixels, y = neighborhood tap count.
    float4 LocalExposureParams;
};

DLSSNR_BLOCK(EasuBlock)
{
    float4 Con0;
    float4 Con1;
    float4 Con2;
};

DLSSNR_BLOCK(RcasBlock)
{
    float Enabled;
    float Strength;
    float Reserved0;
    float Reserved1;
};

DLSSNR_BLOCK(HistogramBlock)
{
    // x = filter flags, y/z = depth range, w = depth inversion.
    float4 AdaptiveFilter;
    float4 Settings;        // x = use 2x2-stride metering samples
};

DLSSNR_BLOCK(TemporalBlock)
{
    float4 Thresholds;           // photometric abs/rel, depth abs/rel
    float4 MotionLimits;         // x = max displacement, y = debug availability
    float4 MotionScaleBase;      // motion scale xy, motion subrect origin zw
    float4 MotionReferenceSize;  // reference dimensions xy, motion subrect size zw
    float4 GuideBaseSize;        // guide subrect origin xy, guide subrect size zw
    float4 HistoryControl;       // x = reuse validated history instead of extracting fresh edit
};

DLSSNR_BLOCK(LocalExposureControlBlock)
{
    float4 Shape;  // shadow transition, highlight transition, bright protection, dark protection
    float4 Detail; // detail retention, shadow strength, highlight strength, reserved
};

DLSSNR_CBUFFER(ComputeConstants)
{
    DLSSNR_UINT Width;
    DLSSNR_UINT Height;
    DLSSNR_UINT pad0;
    DLSSNR_UINT pad1;

    ShaderBlock shader;
    GradeBlock grade;
    ResidualBlock residual;
    HdrBlock hdr;
    EasuBlock easu;
    RcasBlock rcas;
    HistogramBlock histogram;
    TemporalBlock temporal;
    LocalExposureControlBlock localExposure;
};

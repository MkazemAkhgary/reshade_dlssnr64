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
    float Unexpose;
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
    float Bt2446K2;
    float Bt2446K4;
    float Crosstalk;
    // Compose upsample selection. These two slots occupy the cbuffer
    // positions previously held by ReservedGain0/ReservedGain1; the
    // layout is byte-for-byte identical to before, only the names and
    // usage changed.
    //
    // UpsampleMode: 0 = FSR 1.0 EASU, 1 = joint bilateral.
    // UpsampleSharpness: guide-difference exponent for the bilateral
    // path; ignored when mode selects EASU.
    float UpsampleMode;
    float UpsampleSharpness;
    float GainMinLog2;
    float GainMaxLog2;
    // Absolute effective scene-white peak in nits.
    float EffectiveSceneWhiteNits;
    // Edge-aware neighborhood weighting strength for Local Exposure.
    float LocalExposureEdgeSharpness;
    // x = local exposure strength, y = max brighten stops, z = max darken
    // stops, w = model midtone target. Zero strength disables local exposure.
    float4 LocalExposure;
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
    float pad0;
    float pad1;
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
};

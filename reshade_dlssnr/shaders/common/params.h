#ifndef DLSSNR_PARAMS_H
#define DLSSNR_PARAMS_H

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
    float4 Params0;   // kShaderParams[0..3]
    float4 Params1;   // kShaderParams[4..7]
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
    float pad0;
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
    float BaseOffset;
    float AlternateOffset;
    float GainMinLog2;
    float GainMaxLog2;
    // Absolute effective scene-white peak in nits.
    float EffectiveSceneWhiteNits;
    // 0 = current ratio compose, 1 = inverse-tone-map HDR compose.
    float ComposeMode;
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

#endif

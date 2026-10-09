#pragma once
#ifndef DLSSNR_BINDINGS_HLSI
#define DLSSNR_BINDINGS_HLSI

#include "cbuffer.hlsli"


// Grade
#define kGradeEnabled    grade.Enabled
#define kGradeLuma       grade.Luma
#define kGradeChroma     grade.Chroma
#define kGradeLumaMode   grade.LumaMode
#define kGradeChromaMode grade.ChromaMode
#define kGradeBlackLevel grade.BlackLevel
#define kGradeWhiteLevel grade.WhiteLevel
#define kGradeContrast   grade.Contrast
#define kGradeShadows    grade.Shadows
#define kGradeHighlights grade.Highlights
#define kGradeFormat     grade.Format

// Residual edit
#define kEditStrength     residual.EditStrength
#define kResidualChroma   residual.ChromaStrength
#define kEditGamma        residual.EditGamma
#define kHighlightProtect residual.HighlightProtect
#define kShadowProtect    residual.ShadowProtect
#define kRatioEps         residual.RatioEps
#define kRatioBrighten    residual.RatioBrighten
#define kRatioDarken      residual.RatioDarken

// HDR / gain-map
#define kFormat              hdr.Format
#define kExposure            hdr.Exposure
#define kBt2446K1            hdr.Bt2446K1
#define kBt2446K2            hdr.Bt2446K2
#define kBt2446K4            hdr.Bt2446K4
#define kCrosstalk           hdr.Crosstalk
#define kGainMinLog2         hdr.GainMinLog2
#define kGainMaxLog2         hdr.GainMaxLog2
#define kEffectiveSceneWhiteNits hdr.EffectiveSceneWhiteNits

// FSR
#define kEasuCon0     easu.Con0
#define kEasuCon1     easu.Con1
#define kEasuCon2     easu.Con2
#define kRcasEnabled  rcas.Enabled
#define kRcasStrength rcas.Strength

// Dynamic shader parameter table
#define kLumaStrength    shader.Params0.x
#define kChromaStrength  shader.Params0.y
#define kSoftCompression shader.Params0.z
#define kSoftMaxBrighten shader.Params0.w
#define kSoftMaxDarken   shader.Params1.x
#define kSoftMaxChroma   shader.Params1.y
#define kSoftKnee        shader.Params1.z

#endif

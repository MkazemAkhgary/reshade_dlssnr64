#pragma once

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
#define kRestoreExposureFlag grade.RestoreExposure

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
#define kUpsampleMode      hdr.UpsampleMode
#define kUpsampleSharpness hdr.UpsampleSharpness
#define kUpsampleKernelHalf shader.Params1.w

// Temporal reprojection settings and motion/depth guide mapping.
#define kTemporalPhotometricAbsThreshold temporal.Thresholds.x
#define kTemporalPhotometricRelThreshold temporal.Thresholds.y
#define kTemporalDepthAbsThreshold      temporal.Thresholds.z
#define kTemporalDepthRelThreshold      temporal.Thresholds.w
#define kTemporalMaxDisplacement        temporal.MotionLimits.x
#define kTemporalDebugAvailable         temporal.MotionLimits.y
#define kTemporalMotionScale            temporal.MotionScaleBase.xy
#define kTemporalMotionBase             temporal.MotionScaleBase.zw
#define kTemporalMotionReferenceSize    temporal.MotionReferenceSize.xy
#define kTemporalMotionSubrectSize      temporal.MotionReferenceSize.zw
#define kTemporalGuideBase              temporal.GuideBaseSize.xy
#define kTemporalGuideSize              temporal.GuideBaseSize.zw

#pragma once

#include <cstddef>

#include "shaders/common/params.h"

static_assert(sizeof(ShaderBlock) == 32);
static_assert(sizeof(GradeBlock) == 48);
static_assert(sizeof(ResidualBlock) == 32);
static_assert(sizeof(HdrBlock) == 80);
static_assert(sizeof(HistogramBlock) == 32);
static_assert(sizeof(TemporalBlock) == 96);
static_assert(sizeof(LocalExposureControlBlock) == 32);

constexpr size_t kHeaderSize = 16;
constexpr size_t kShaderOffset = kHeaderSize;
constexpr size_t kGradeOffset = kShaderOffset + sizeof(ShaderBlock);
constexpr size_t kResidualOffset = kGradeOffset + sizeof(GradeBlock);
constexpr size_t kHdrOffset = kResidualOffset + sizeof(ResidualBlock);
constexpr size_t kHistogramOffset = kHdrOffset + sizeof(HdrBlock);
constexpr size_t kTemporalOffset = kHistogramOffset + sizeof(HistogramBlock);
constexpr size_t kLocalExposureOffset = kTemporalOffset + sizeof(TemporalBlock);

static_assert(offsetof(ComputeConstants, shader) == kShaderOffset);
static_assert(offsetof(ComputeConstants, grade) == kGradeOffset);
static_assert(offsetof(ComputeConstants, residual) == kResidualOffset);
static_assert(offsetof(ComputeConstants, hdr) == kHdrOffset);
static_assert(offsetof(ComputeConstants, histogram) == kHistogramOffset);
static_assert(offsetof(ComputeConstants, temporal) == kTemporalOffset);
static_assert(offsetof(ComputeConstants, localExposure) == kLocalExposureOffset);

static_assert(kHistogramOffset == 208);
static_assert(kTemporalOffset == 240);
static_assert(kLocalExposureOffset == 336);
static_assert(sizeof(ComputeConstants) == 512);

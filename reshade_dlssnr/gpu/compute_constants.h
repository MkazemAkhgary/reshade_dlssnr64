#pragma once

#include <cstddef>

#include "shaders/common/params.h"

static_assert(sizeof(ShaderBlock) == 32);
static_assert(sizeof(GradeBlock) == 48);
static_assert(sizeof(ResidualBlock) == 32);
static_assert(sizeof(HdrBlock) == 64);
static_assert(sizeof(EasuBlock) == 48);
static_assert(sizeof(RcasBlock) == 16);
static_assert(sizeof(TemporalBlock) == 80);

constexpr size_t kHeaderSize = 16;
constexpr size_t kShaderOffset = kHeaderSize;
constexpr size_t kGradeOffset = kShaderOffset + sizeof(ShaderBlock);
constexpr size_t kResidualOffset = kGradeOffset + sizeof(GradeBlock);
constexpr size_t kHdrOffset = kResidualOffset + sizeof(ResidualBlock);
constexpr size_t kEasuOffset = kHdrOffset + sizeof(HdrBlock);
constexpr size_t kRcasOffset = kEasuOffset + sizeof(EasuBlock);
constexpr size_t kTemporalOffset = kRcasOffset + sizeof(RcasBlock);

static_assert(offsetof(ComputeConstants, shader) == kShaderOffset);
static_assert(offsetof(ComputeConstants, grade) == kGradeOffset);
static_assert(offsetof(ComputeConstants, residual) == kResidualOffset);
static_assert(offsetof(ComputeConstants, hdr) == kHdrOffset);
static_assert(offsetof(ComputeConstants, easu) == kEasuOffset);
static_assert(offsetof(ComputeConstants, rcas) == kRcasOffset);
static_assert(offsetof(ComputeConstants, temporal) == kTemporalOffset);

static_assert(kTemporalOffset == 256);
static_assert(sizeof(ComputeConstants) == 512);

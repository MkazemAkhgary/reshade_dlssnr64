#include "compute_passes.h"

#include <cstring>

#include "compose/shaders.h"
#include "debug/shaders.h"
#include "grade/shaders.h"
#include "hdr/shaders.h"
#include "model/shaders.h"
#include "skin/shaders.h"

namespace reshade_dlssnr {

const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",         kZeroHistogramSource,     nullptr, nullptr},
    {"BuildHistogram",        kBuildHistogramSource,    nullptr, nullptr},
    {"GainMapSplit",          kGainMapSplitSource,      kBoxAreaAveragePreludeSource, nullptr},
    {"ZeroGainMapStats",      kZeroGainMapStatsSource,  nullptr, nullptr},
    {"BuildGainMapStats",     kBuildGainMapStatsSource, nullptr, nullptr},
    {"PrepareModelInput",     kPrepareModelInputSource, kBoxAreaAveragePreludeSource, nullptr},
    {"ComposeModelOutput",    kComposeModelOutputSource,    kGradePreludeSource, kFsrUpsamplePreludeSource},
    {"ComposeModelOutputHdr", kComposeModelOutputHdrSource, kGradePreludeSource, kFsrUpsamplePreludeSource},
    {"SkinMaskDiff",          kSkinMaskDiffSource,    nullptr, nullptr},
    {"DebugDisplay",          kDebugDisplaySource,    nullptr, nullptr},
    {"ApplyGrade",            kApplyGradeSource,      kGradePreludeSource, nullptr},
};

const uint32_t kComputePassCount =
    sizeof(kComputePasses) / sizeof(kComputePasses[0]);

const ComputePassDef* FindComputePassDef(const char* name) {
  if (name == nullptr) return nullptr;
  for (uint32_t i = 0; i < kComputePassCount; ++i) {
    if (std::strcmp(kComputePasses[i].name, name) == 0) {
      return &kComputePasses[i];
    }
  }
  return nullptr;
}

const char* FindComputePassSource(const char* name) {
  const ComputePassDef* def = FindComputePassDef(name);
  return def != nullptr ? def->hlsl_source : nullptr;
}

}  // namespace reshade_dlssnr

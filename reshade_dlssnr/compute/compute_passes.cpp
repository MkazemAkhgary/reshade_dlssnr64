#include "compute_passes.h"

#include <cstring>

#include "shaders/compose.h"
#include "shaders/debug.h"
#include "shaders/grade.h"
#include "shaders/hdr.h"
#include "shaders/model.h"
#include "shaders/skin.h"

namespace reshade_dlssnr {

namespace {









}  // namespace


const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",         kZeroHistogramSource,     nullptr},
    {"BuildHistogram",        kBuildHistogramSource,    nullptr},
    {"TonemapForward",        kTonemapForwardSource,    nullptr},
    {"PrepareModelInput",     kPrepareModelInputSource, nullptr},
    {"ComposeModelOutput",    kComposeModelOutputSource,    kGradePreludeSource},
    {"ComposeModelOutputHdr", kComposeModelOutputHdrSource, kGradePreludeSource},
    {"SkinMaskDiff",          kSkinMaskDiffSource,    nullptr},
    {"DebugDisplay",          kDebugDisplaySource,    nullptr},
    {"ApplyGrade",            kApplyGradeSource,      kGradePreludeSource},
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

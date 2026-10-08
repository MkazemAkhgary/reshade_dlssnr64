#include "compute_passes.h"

#include <cstring>

#include "generated/shader_bytecode.h"

namespace reshade_dlssnr {

namespace {
const unsigned char* BytecodeData(const char* root) {
  const EmbeddedShaderBytecode* bc = FindEmbeddedShaderBytecode(root);
  return bc != nullptr ? bc->data : nullptr;
}
std::size_t BytecodeSize(const char* root) {
  const EmbeddedShaderBytecode* bc = FindEmbeddedShaderBytecode(root);
  return bc != nullptr ? bc->size : 0;
}
}  // namespace

const ComputePassDef kComputePasses[] = {
    {"ZeroHistogram",         "hdr/zero_histogram.hlsl",        BytecodeData("hdr/zero_histogram.hlsl"),        BytecodeSize("hdr/zero_histogram.hlsl")},
    {"BuildHistogram",        "hdr/build_histogram.hlsl",       BytecodeData("hdr/build_histogram.hlsl"),       BytecodeSize("hdr/build_histogram.hlsl")},
    {"GainMapSplit",          "hdr/gain_map_split.hlsl",        BytecodeData("hdr/gain_map_split.hlsl"),        BytecodeSize("hdr/gain_map_split.hlsl")},
    {"BuildEffectiveLocalExposure", "hdr/build_effective_local_exposure.hlsl", BytecodeData("hdr/build_effective_local_exposure.hlsl"), BytecodeSize("hdr/build_effective_local_exposure.hlsl")},
    {"ZeroGainMapStats",      "hdr/zero_gain_map_stats.hlsl",   BytecodeData("hdr/zero_gain_map_stats.hlsl"),   BytecodeSize("hdr/zero_gain_map_stats.hlsl")},
    {"BuildGainMapStats",     "hdr/build_gain_map_stats.hlsl",  BytecodeData("hdr/build_gain_map_stats.hlsl"),  BytecodeSize("hdr/build_gain_map_stats.hlsl")},
    {"ReadGameExposure",      "hdr/read_game_exposure.hlsl",   BytecodeData("hdr/read_game_exposure.hlsl"),   BytecodeSize("hdr/read_game_exposure.hlsl")},
    {"PrepareModelInput",     "model/prepare_model_input.hlsl", BytecodeData("model/prepare_model_input.hlsl"), BytecodeSize("model/prepare_model_input.hlsl")},
    {"ComposeModelOutput",    "compose/compose_model_output.hlsl", BytecodeData("compose/compose_model_output.hlsl"), BytecodeSize("compose/compose_model_output.hlsl")},
    {"ComposeModelOutputHdr", "compose/compose_model_output_hdr.hlsl", BytecodeData("compose/compose_model_output_hdr.hlsl"), BytecodeSize("compose/compose_model_output_hdr.hlsl")},
    {"ComposeModelOutputHdrInverse", "compose/compose_model_output_hdr_inverse.hlsl", BytecodeData("compose/compose_model_output_hdr_inverse.hlsl"), BytecodeSize("compose/compose_model_output_hdr_inverse.hlsl")},
    {"SkinMaskDiff",          "skin/skin_mask_diff.hlsl",      BytecodeData("skin/skin_mask_diff.hlsl"),      BytecodeSize("skin/skin_mask_diff.hlsl")},
    {"DebugDisplay",          "debug/debug_display.hlsl",      BytecodeData("debug/debug_display.hlsl"),      BytecodeSize("debug/debug_display.hlsl")},
    {"DebugHdrRoundtrip",    "debug/debug_hdr_roundtrip.hlsl", BytecodeData("debug/debug_hdr_roundtrip.hlsl"), BytecodeSize("debug/debug_hdr_roundtrip.hlsl")},
    {"ApplyGrade",            "grade/apply_grade.hlsl",        BytecodeData("grade/apply_grade.hlsl"),        BytecodeSize("grade/apply_grade.hlsl")},
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

}  // namespace reshade_dlssnr

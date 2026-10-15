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
    {"TemporalClearStats", "temporal/clear_stats.hlsl", BytecodeData("temporal/clear_stats.hlsl"), BytecodeSize("temporal/clear_stats.hlsl")},
    {"TemporalExtractEdit", "temporal/extract_edit.hlsl", BytecodeData("temporal/extract_edit.hlsl"), BytecodeSize("temporal/extract_edit.hlsl")},
    {"TemporalReprojectEdit", "temporal/reproject_edit.hlsl", BytecodeData("temporal/reproject_edit.hlsl"), BytecodeSize("temporal/reproject_edit.hlsl")},
    {"TemporalValidateDepth", "temporal/validate_depth.hlsl", BytecodeData("temporal/validate_depth.hlsl"), BytecodeSize("temporal/validate_depth.hlsl")},
    {"TemporalCountInvalid", "temporal/count_invalid.hlsl", BytecodeData("temporal/count_invalid.hlsl"), BytecodeSize("temporal/count_invalid.hlsl")},
    {"TemporalApplyEdit", "temporal/apply_edit.hlsl", BytecodeData("temporal/apply_edit.hlsl"), BytecodeSize("temporal/apply_edit.hlsl")},
    {"TemporalCommitEdit", "temporal/commit_edit.hlsl", BytecodeData("temporal/commit_edit.hlsl"), BytecodeSize("temporal/commit_edit.hlsl")},
    {"TemporalStoreDepth", "temporal/store_depth.hlsl", BytecodeData("temporal/store_depth.hlsl"), BytecodeSize("temporal/store_depth.hlsl")},
    {"ZeroHistogram",         "hdr/zero_histogram.hlsl",        BytecodeData("hdr/zero_histogram.hlsl"),        BytecodeSize("hdr/zero_histogram.hlsl")},
    {"BuildHistogram",        "hdr/build_histogram.hlsl",       BytecodeData("hdr/build_histogram.hlsl"),       BytecodeSize("hdr/build_histogram.hlsl")},
    {"GainMapSplit",          "hdr/gain_map_split.hlsl",        BytecodeData("hdr/gain_map_split.hlsl"),        BytecodeSize("hdr/gain_map_split.hlsl")},
    {"BuildEffectiveLocalExposure", "hdr/build_effective_local_exposure.hlsl", BytecodeData("hdr/build_effective_local_exposure.hlsl"), BytecodeSize("hdr/build_effective_local_exposure.hlsl")},
    {"ReduceLocalExposureTiles", "hdr/reduce_local_exposure_tiles.hlsl", BytecodeData("hdr/reduce_local_exposure_tiles.hlsl"), BytecodeSize("hdr/reduce_local_exposure_tiles.hlsl")},
    {"ReduceLocalExposureMean", "hdr/reduce_local_exposure_mean.hlsl", BytecodeData("hdr/reduce_local_exposure_mean.hlsl"), BytecodeSize("hdr/reduce_local_exposure_mean.hlsl")},
    {"ZeroGainMapStats",      "hdr/zero_gain_map_stats.hlsl",   BytecodeData("hdr/zero_gain_map_stats.hlsl"),   BytecodeSize("hdr/zero_gain_map_stats.hlsl")},
    {"BuildGainMapStats",     "hdr/build_gain_map_stats.hlsl",  BytecodeData("hdr/build_gain_map_stats.hlsl"),  BytecodeSize("hdr/build_gain_map_stats.hlsl")},
    {"ReadGameExposure",      "hdr/read_game_exposure.hlsl",   BytecodeData("hdr/read_game_exposure.hlsl"),   BytecodeSize("hdr/read_game_exposure.hlsl")},
    {"PrepareModelInput",     "model/prepare_model_input.hlsl", BytecodeData("model/prepare_model_input.hlsl"), BytecodeSize("model/prepare_model_input.hlsl")},
    {"ComposeModelOutput",    "compose/compose_model_output.hlsl", BytecodeData("compose/compose_model_output.hlsl"), BytecodeSize("compose/compose_model_output.hlsl")},
    {"ComposeModelOutputHdr", "compose/compose_model_output_hdr.hlsl", BytecodeData("compose/compose_model_output_hdr.hlsl"), BytecodeSize("compose/compose_model_output_hdr.hlsl")},
    {"BuildImportanceMask",          "importance_mask/build_importance_mask.hlsl",      BytecodeData("importance_mask/build_importance_mask.hlsl"),      BytecodeSize("importance_mask/build_importance_mask.hlsl")},
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

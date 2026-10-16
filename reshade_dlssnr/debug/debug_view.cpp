#include "debug/debug_view.h"

#include <algorithm>

#include <wrl/client.h>

#include "pipeline/pipeline_graph.h"

namespace reshade_dlssnr {

namespace {

ID3D12Resource* SelectSource(const DebugSnapshot& s,
                             unsigned int* w, unsigned int* h) {
  *w = s.source_width;
  *h = s.source_height;
  return s.source;
}

ID3D12Resource* SelectPackedSdr(const DebugSnapshot& s,
                                unsigned int* w, unsigned int* h) {
  *w = s.packed_width;
  *h = s.packed_height;
  return s.packed_hdr_sdr;
}

ID3D12Resource* SelectGainMap(const DebugSnapshot& s,
                              unsigned int* w, unsigned int* h) {
  *w = s.packed_width;
  *h = s.packed_height;
  return s.packed_hdr_gain;
}

ID3D12Resource* SelectPreparedInput(const DebugSnapshot& s,
                                    unsigned int* w, unsigned int* h) {
  *w = s.prepared_input_width;
  *h = s.prepared_input_height;
  return s.prepared_input;
}

ID3D12Resource* SelectPreGradeInput(const DebugSnapshot& s,
                                    unsigned int* w, unsigned int* h) {
  *w = s.pre_grade_input_width;
  *h = s.pre_grade_input_height;
  return s.pre_grade_input;
}

ID3D12Resource* SelectPreGradeOutput(const DebugSnapshot& s,
                                     unsigned int* w, unsigned int* h) {
  *w = s.pre_grade_output_width;
  *h = s.pre_grade_output_height;
  return s.pre_grade_output;
}

ID3D12Resource* SelectPostGradeOutput(const DebugSnapshot& s,
                                      unsigned int* w, unsigned int* h) {
  *w = s.model_output_width;
  *h = s.model_output_height;
  return s.model_output;
}

ID3D12Resource* SelectComposedOutput(const DebugSnapshot& s,
                                     unsigned int* w, unsigned int* h) {
  *w = s.composed_output_width;
  *h = s.composed_output_height;
  return s.composed_output;
}

ID3D12Resource* SelectMotion(const DebugSnapshot& s,
                             unsigned int* w, unsigned int* h) {
  *w = s.motion_width;
  *h = s.motion_height;
  return s.motion;
}

ID3D12Resource* SelectDepth(const DebugSnapshot& s,
                            unsigned int* w, unsigned int* h) {
  *w = s.depth_width;
  *h = s.depth_height;
  return s.depth;
}

ID3D12Resource* SelectImportanceMask(const DebugSnapshot& s,
                                    unsigned int* w, unsigned int* h) {
  *w = s.importance_width;
  *h = s.importance_height;
  return s.importance_mask;
}

ID3D12Resource* SelectTemporalCandidate(const DebugSnapshot& s,
                                       unsigned int* w, unsigned int* h) {
  if (s.temporal_candidate != nullptr) {
    *w = s.temporal_width;
    *h = s.temporal_height;
    return s.temporal_candidate;
  }
  *w = s.source_width;
  *h = s.source_height;
  return s.source;
}

ID3D12Resource* SelectTemporalValidatedCandidate(const DebugSnapshot& s,
                                                 unsigned int* w,
                                                 unsigned int* h) {
  if (s.temporal_validated_candidate != nullptr) {
    *w = s.temporal_width;
    *h = s.temporal_height;
    return s.temporal_validated_candidate;
  }
  *w = s.source_width;
  *h = s.source_height;
  return s.source;
}

}  // namespace

const DebugViewDef kDebugViews[] = {
    {"Model Input (source)",
     "The frame as captured, before any processing. This is the DLSS "
     "output the model will work from, at its native resolution.",
     DebugDisplayMode::Color, SelectSource},
    {"Model Input (pre-grade)",
     "The model input color exactly as it was before the Input Grade ran. "
     "The grade is applied in place, so the pipeline snapshots this texture "
     "for you when the view is selected; with the Input Grade enabled, "
     "compare it against Model Input (prepared).",
     DebugDisplayMode::Color, SelectPreGradeInput, true, false},
    {"Model Input (prepared)",
     "The image actually fed to the model, after resizing to the Working "
     "Scale, HDR preparation, and input grading when enabled.",
     DebugDisplayMode::Color, SelectPreparedInput},
    {"Model Output (raw)",
     "The model's output exactly as NGX produced it, before the Output Grade "
     "is applied. Snapshot taken by the pipeline because the grade runs in "
     "place; with the Output Grade enabled, compare it against Model Output "
     "(post-grade).",
     DebugDisplayMode::Color, SelectPreGradeOutput, false, true},
    {"Model Output (post-grade)",
     "The model output after the Output Grade ran, in the model's own "
     "sRGB-encoded SDR domain - symmetric to the Input Grade on the model "
     "input. Everything downstream (compose, importance mask, writeback) "
     "consumes this as if the model had produced it.",
     DebugDisplayMode::Color, SelectPostGradeOutput, false, false},
    {"Model Output (composed)",
     "The final result of this pass, after the model's edit has been "
     "composed with the source. This is what gets written to the target.",
     DebugDisplayMode::Color, SelectComposedOutput},
    {"Motion Vectors",
     "The motion vectors the game supplied to DLSS. Shown as R = x "
     "direction, G = y direction.",
     DebugDisplayMode::Motion, SelectMotion},
    {"Depth",
     "The depth buffer the game supplied to DLSS. Shown as grayscale.",
     DebugDisplayMode::Depth, SelectDepth},
    {"Importance Mask",
     "The current frame's mask from the actual model input (including input grading and Local Exposure) versus the raw model output. It is published for exposure use on the next frame.",
     DebugDisplayMode::Mask, SelectImportanceMask},
    {"SDR Base (pre-model)",
     "Full-resolution linear SDR carrier produced by the BT.2446C split. "
     "This is the RGB payload the model is downsampled from.",
     DebugDisplayMode::PackedSdr, SelectPackedSdr},
    {"Gain Map",
     "Raw f32 carrier-to-HDR reconstruction multiplier, displayed in log2 stops from -4 (black) to +6 (white).",
     DebugDisplayMode::GainMap, SelectGainMap},
    {"HDR Roundtrip Diagnostic",
     "2x2 comparison: actual prepared Model Input | Model Output / composed HDR "
     "mapped back to the ungraded model domain | composition difference against "
     "the ungraded Model Output. Nonzero edges can be expected because full-resolution "
     "source detail is preserved; working scale and RCAS affect this comparison. "
     "Use HDR Roundtrip Error for the isolated carrier reconstruction check.",
     DebugDisplayMode::HdrRoundtripDiagnostic, SelectComposedOutput},
    {"HDR Roundtrip Error",
     "False-color relative error between the original decoded HDR source and the "
     "direct float32 reconstruction from the RGBA32F carrier and carrier-to-HDR multiplier. "
     "This validates the carrier transfer only, not the model edit or final composition; "
     "independent of Working Scale, RCAS, and input grade.",
     DebugDisplayMode::HdrRoundtripError, SelectPackedSdr},
    {"HDR Local Exposure",
     "Local exposure factor in stops, mapped so 0 stops is mid-gray, -2 stops "
     "is black, and +2 stops is white.",
     DebugDisplayMode::HdrLocalExposure, SelectSource},
    {"Game Exposure",
     "Game NGX exposure in stops: 1.0x (0 stops) is mid-gray; brighter/darker "
     "values move toward white/black. Magenta means no valid value was read.",
     DebugDisplayMode::GameExposure, SelectSource},
    {"Temporal Candidate Status",
     "Status of the motion-reprojected prior edit before depth validation: "
     "red = rejected motion/history/coordinates, green = eligible, yellow = "
     "photometric mismatch. Unavailable until a previous edit can be reprojected.",
     DebugDisplayMode::TemporalCandidateStatus, SelectTemporalCandidate},
    {"Temporal Validation Status",
     "Status after depth validation: red = no candidate, green = accepted, "
     "yellow = photometric mismatch, magenta = invalid depth or guide mapping.",
     DebugDisplayMode::TemporalValidationStatus, SelectTemporalValidatedCandidate},
    {"Temporal Reprojected Edit",
     "Validated per-channel log-gain from the previous frame, mapped so "
     "neutral gain is gray and stronger positive/negative edits move toward "
     "white/black. Rejected pixels are dark.",
     DebugDisplayMode::TemporalEdit, SelectTemporalValidatedCandidate},
    {nullptr, nullptr, DebugDisplayMode::Color, nullptr},
};

const uint32_t CountDebugViews() {
  uint32_t count = 0;
  while (kDebugViews[count].name != nullptr) ++count;
  return count;
}

bool DebugViewNeedsPreGradeInput(int debug_view) {
  if (debug_view <= 0) return false;
  const uint32_t index = static_cast<uint32_t>(debug_view) - 1;
  if (index >= CountDebugViews()) return false;
  return kDebugViews[index].needs_pre_grade_input;
}

bool DebugViewNeedsPreGradeOutput(int debug_view) {
  if (debug_view <= 0) return false;
  const uint32_t index = static_cast<uint32_t>(debug_view) - 1;
  if (index >= CountDebugViews()) return false;
  return kDebugViews[index].needs_pre_grade_output;
}

bool DebugViewNeedsEffectiveExposureMap(int debug_view) {
  if (debug_view <= 0) return false;
  const uint32_t index = static_cast<uint32_t>(debug_view) - 1;
  if (index >= CountDebugViews()) return false;
  const DebugDisplayMode mode = kDebugViews[index].display_mode;
  return mode == DebugDisplayMode::HdrLocalExposure ||
         mode == DebugDisplayMode::HdrRoundtripDiagnostic;
}

bool RenderDebugView(
    CommandContext& state,
    ComputeEngine& codec,
    ID3D12GraphicsCommandList* command_list,
    const DebugSnapshot& snapshot,
    const model_pipeline::HdrUniforms* hdr,
    int debug_view,
    ID3D12Resource* target,
    unsigned int target_width,
    unsigned int target_height) {
  if (debug_view <= 0 || command_list == nullptr || target == nullptr ||
      target_width == 0 || target_height == 0) {
    return false;
  }
  // The debug shader is compiled on the worker like everything else;
  // skipping a frame here just leaves the debug view one frame behind.
  if (!codec.IsReady()) return false;

  const uint32_t index = static_cast<uint32_t>(debug_view) - 1;
  if (index >= CountDebugViews()) return false;

  unsigned int content_w = 0;
  unsigned int content_h = 0;
  ID3D12Resource* source =
      kDebugViews[index].select(snapshot, &content_w, &content_h);
  if (source == nullptr || content_w == 0 || content_h == 0) {
    return false;
  }

  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();

  const DebugDisplayMode display_mode = kDebugViews[index].display_mode;
  const bool hdr_diag = display_mode == DebugDisplayMode::HdrRoundtripDiagnostic ||
                        display_mode == DebugDisplayMode::HdrRoundtripError ||
                        display_mode == DebugDisplayMode::HdrLocalExposure;
  const bool hdr_needs_model =
      display_mode == DebugDisplayMode::HdrRoundtripDiagnostic;
  const bool hdr_needs_carrier =
      display_mode == DebugDisplayMode::HdrRoundtripError;
  const bool hdr_needs_composed =
      display_mode == DebugDisplayMode::HdrRoundtripDiagnostic;
  if (hdr_diag &&
      (hdr == nullptr || snapshot.source == nullptr ||
       (hdr_needs_composed && snapshot.composed_output == nullptr) ||
       (hdr_needs_model && (snapshot.prepared_input == nullptr ||
                            snapshot.model_output == nullptr)) ||
       (hdr_needs_carrier && snapshot.packed_hdr_sdr == nullptr))) {
    return false;
  }

  ComputeConstants constants = {};
  constants.Width = target_width;
  constants.Height = target_height;
  constants.shader.Params0.x = static_cast<float>(
      static_cast<uint32_t>(kDebugViews[index].display_mode));
  constants.shader.Params0.y = static_cast<float>(content_w);
  constants.shader.Params0.z = static_cast<float>(content_h);
  constants.shader.Params0.w = static_cast<float>(
      static_cast<unsigned int>(source_desc.Width));
  constants.shader.Params1.x = static_cast<float>(
      static_cast<unsigned int>(source_desc.Height));
  constants.shader.Params1.y = snapshot.game_exposure;
  constants.shader.Params1.z = snapshot.game_exposure_available ? 1.0f : 0.0f;
  constants.temporal.MotionLimits.y = snapshot.temporal_available ? 1.0f : 0.0f;

  if (hdr_diag) {
    constants.hdr.Format = hdr->format;
    constants.hdr.Exposure = hdr->exposure;
    constants.hdr.Bt2446K1 = hdr->bt2446_k1;
    constants.hdr.Bt2446K2 = hdr->bt2446_k2;
    constants.hdr.Bt2446K4 = hdr->bt2446_k4;
    constants.hdr.Crosstalk = hdr->bt2446_crosstalk;
    constants.hdr.GainMinLog2 = hdr->gain_min_log2;
    constants.hdr.GainMaxLog2 = hdr->gain_max_log2;
    constants.hdr.EffectiveSceneWhiteNits = hdr->effective_scene_white_nits;
    constants.hdr.LocalExposure.x = hdr->local_exposure_strength;
    constants.hdr.LocalExposure.y = hdr->local_max_brighten;
    constants.hdr.LocalExposure.z = hdr->local_max_darken;
    constants.hdr.LocalExposure.w = hdr->midtone;

    const GradeSettings& grade = snapshot.input_grade;
    constants.grade.Enabled = grade.enabled ? 1.0f : 0.0f;
    constants.grade.Luma = grade.luma;
    constants.grade.Chroma = grade.chroma;
    constants.grade.LumaMode = static_cast<float>(static_cast<int>(grade.luma_mode));
    constants.grade.ChromaMode = static_cast<float>(static_cast<int>(grade.chroma_mode));
    constants.grade.BlackLevel = grade.black_level;
    constants.grade.WhiteLevel = grade.white_level;
    constants.grade.Contrast = grade.contrast;
    constants.grade.Shadows = grade.shadows;
    constants.grade.Highlights = grade.highlights;

    const D3D12_RESOURCE_DESC model_desc =
        hdr_needs_model && snapshot.prepared_input != nullptr
            ? snapshot.prepared_input->GetDesc()
            : (snapshot.model_output != nullptr
                   ? snapshot.model_output->GetDesc()
                   : source_desc);
    const float in_w = static_cast<float>(std::max<UINT>(1u, static_cast<UINT>(model_desc.Width)));
    const float in_h = static_cast<float>(std::max<UINT>(1u, model_desc.Height));
    const float out_w = static_cast<float>(std::max(1u, target_width));
    const float out_h = static_cast<float>(std::max(1u, target_height));
    constants.easu.Con0.x = in_w / out_w;
    constants.easu.Con0.y = in_h / out_h;
    constants.easu.Con0.z = 0.5f * in_w / out_w - 0.5f;
    constants.easu.Con0.w = 0.5f * in_h / out_h - 0.5f;
    constants.easu.Con1.x = 1.0f / in_w;
    constants.easu.Con1.y = 1.0f / in_h;
    constants.easu.Con1.z = 1.0f / in_w;
    constants.easu.Con1.w = -1.0f / in_h;
    constants.easu.Con2.x = -1.0f / in_w;
    constants.easu.Con2.y = 2.0f / in_h;
    constants.easu.Con2.z = 1.0f / in_w;
    constants.easu.Con2.w = 2.0f / in_h;

    constants.rcas.Enabled = snapshot.rcas_enabled ? 1.0f : 0.0f;
    constants.rcas.Strength = snapshot.rcas_strength;

    if (snapshot.composed_output != nullptr) {
      state.Transition(command_list, snapshot.composed_output,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    ID3D12Resource* model_input_resource = hdr_needs_model
        ? snapshot.prepared_input : snapshot.source;
    state.Transition(command_list, model_input_resource,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* auxiliary_resource = hdr_needs_carrier
        ? snapshot.packed_hdr_sdr : snapshot.model_output;
    if (auxiliary_resource != nullptr) {
      state.Transition(command_list, auxiliary_resource,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (snapshot.effective_exposure_map != nullptr) {
      state.Transition(command_list, snapshot.effective_exposure_map,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    state.Transition(command_list, target,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12Resource* srvs[kSrvCount] = {
        snapshot.composed_output != nullptr ? snapshot.composed_output : snapshot.source,
        model_input_resource,
        auxiliary_resource != nullptr ? auxiliary_resource : snapshot.source,
        snapshot.effective_exposure_map != nullptr
            ? snapshot.effective_exposure_map : snapshot.source};
    constants.shader.Params1.w =
        snapshot.effective_exposure_map != nullptr ? 1.0f : 0.0f;
    ID3D12Resource* uavs[kUavCount] = {target};
    const uint32_t diag_mode =
        display_mode == DebugDisplayMode::HdrRoundtripDiagnostic ? 0u :
        display_mode == DebugDisplayMode::HdrRoundtripError ? 1u : 2u;
    constants.shader.Params0.x = static_cast<float>(diag_mode);
    return codec.Dispatch("DebugHdrRoundtrip", command_list, constants, srvs, uavs);
  }

  state.Transition(command_list, source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {source, source, source, source};
  ID3D12Resource* uavs[kUavCount] = {target};

  return codec.Dispatch("DebugDisplay", command_list, constants, srvs, uavs);
}

// ---------------------------------------------------------------------------
// Pipeline step registration
// ---------------------------------------------------------------------------
//
// This translation unit owns no pipeline steps. The RenderDebugView node is
// implemented in pass_session.cpp, because rendering the debug view requires
// the pass's own state: the DebugSnapshot of live resources, which only the
// pass can populate, and the FrameDebugContext::output (re)creation that has
// to happen while those resources are in known states.
//
// Declared anyway so the graph has a definition for every node slot.
const PipelineStepDef* DebugStepFor(PipelineNodeId) { return nullptr; }

}  // namespace reshade_dlssnr

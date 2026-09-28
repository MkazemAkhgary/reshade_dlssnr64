#include "debug_view.h"

#include <wrl/client.h>

#include "pipeline_graph.h"

namespace reshade_dlssnr {

namespace {

ID3D12Resource* SelectSource(const DebugSnapshot& s,
                             unsigned int* w, unsigned int* h) {
  *w = s.source_width;
  *h = s.source_height;
  return s.source;
}

ID3D12Resource* SelectPreparedInput(const DebugSnapshot& s,
                                    unsigned int* w, unsigned int* h) {
  *w = s.prepared_input_width;
  *h = s.prepared_input_height;
  return s.prepared_input;
}

ID3D12Resource* SelectModelOutput(const DebugSnapshot& s,
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

ID3D12Resource* SelectSkinDetectInput(const DebugSnapshot& s,
                                      unsigned int* w, unsigned int* h) {
  *w = s.skin_detect_width;
  *h = s.skin_detect_height;
  return s.skin_detect_input;
}

ID3D12Resource* SelectSkinDetectOutput(const DebugSnapshot& s,
                                       unsigned int* w, unsigned int* h) {
  *w = s.skin_detect_width;
  *h = s.skin_detect_height;
  return s.skin_detect_output;
}

ID3D12Resource* SelectSkinMask(const DebugSnapshot& s,
                               unsigned int* w, unsigned int* h) {
  *w = s.skin_detect_width;
  *h = s.skin_detect_height;
  return s.skin_mask;
}

}  // namespace

const DebugViewDef kDebugViews[] = {
    {"Model Input (source)",
     "The frame as captured, before any processing. This is the DLSS "
     "output the model will work from, at its native resolution.",
     DebugDisplayMode::Color, SelectSource},
    {"Model Input (prepared)",
     "The image actually fed to the model, after resizing to the Working "
     "Scale (and after HDR preparation, when enabled).",
     DebugDisplayMode::Color, SelectPreparedInput},
    {"Model Output (raw)",
     "The model's output, before any of the pipeline's post-processing is "
     "applied. Useful for seeing exactly what the model produced.",
     DebugDisplayMode::Color, SelectModelOutput},
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
    {"Skin Detect Input",
     "The image the skin-detection pass fed to the model (after HDR "
     "normalization and resize).",
     DebugDisplayMode::Color, SelectSkinDetectInput},
    {"Skin Detect Output",
     "The model's raw output from the skin-detection pass. The mask is "
     "derived from the difference against the input.",
     DebugDisplayMode::Color, SelectSkinDetectOutput},
    {"Skin Mask",
     "White = the model classified the pixel as skin. This is what gates "
     "the HDR skin histogram.",
     DebugDisplayMode::Mask, SelectSkinMask},
    {nullptr, nullptr, DebugDisplayMode::Color, nullptr},
};

const uint32_t CountDebugViews() {
  uint32_t count = 0;
  while (kDebugViews[count].name != nullptr) ++count;
  return count;
}

bool RenderDebugView(
    CommandContext& state,
    ComputeEngine& codec,
    ID3D12GraphicsCommandList* command_list,
    const DebugSnapshot& snapshot,
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

  ComputeConstants constants = {};
  constants.Width = target_width;
  constants.Height = target_height;
  constants.params[0] = static_cast<float>(
      static_cast<uint32_t>(kDebugViews[index].display_mode));
  constants.params[1] = static_cast<float>(content_w);
  constants.params[2] = static_cast<float>(content_h);
  constants.params[3] = static_cast<float>(
      static_cast<unsigned int>(source_desc.Width));
  constants.params[4] = static_cast<float>(
      static_cast<unsigned int>(source_desc.Height));

  state.Transition(command_list, source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {source, source, source};
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

#include "debug_view.h"

#include <wrl/client.h>

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

}  // namespace

const DebugViewDef kDebugViews[] = {
    {"Model Input (source)",
     "The frame before any model processing. At DLSS Output this is the "
     "captured DLSS result as the model sees it, including resolution.",
     DebugDisplayMode::Color, SelectSource},
    {"Model Input (prepared)",
     "What the model actually receives: the source resized to the working "
     "scale.",
     DebugDisplayMode::Color, SelectPreparedInput},
    {"Model Output (raw)",
     "The model's raw output before composition. Post-processing sliders "
     "do not affect this view.",
     DebugDisplayMode::Color, SelectModelOutput},
    {"Model Output (composed)",
     "The composed result after residual, soft compression, and luma/chroma "
     "strength. This is what gets written to the target.",
     DebugDisplayMode::Color, SelectComposedOutput},
    {"Motion Vectors",
     "Motion vectors captured from the game's DLSS evaluation. Displayed as "
     "R = x, G = y.",
     DebugDisplayMode::Motion, SelectMotion},
    {"Depth",
     "Depth buffer captured from the game's DLSS evaluation. Displayed as "
     "grayscale (red channel replicated).",
     DebugDisplayMode::Depth, SelectDepth},
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

}  // namespace reshade_dlssnr

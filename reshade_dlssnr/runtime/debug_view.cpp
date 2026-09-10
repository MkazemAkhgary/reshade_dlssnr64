#include "debug_view.h"

#include <wrl/client.h>

namespace reshade_dlssnr {

namespace {

ID3D12Resource* SelectSource(const DebugSnapshot& s,
                             unsigned int* w, unsigned int* h,
                             D3D12_RESOURCE_STATES* st) {
  *w = s.source_width;
  *h = s.source_height;
  *st = s.source_state;
  return s.source;
}

ID3D12Resource* SelectPreparedInput(const DebugSnapshot& s,
                                    unsigned int* w, unsigned int* h,
                                    D3D12_RESOURCE_STATES* st) {
  *w = s.prepared_input_width;
  *h = s.prepared_input_height;
  *st = s.prepared_input_state;
  return s.prepared_input;
}

ID3D12Resource* SelectModelOutput(const DebugSnapshot& s,
                                  unsigned int* w, unsigned int* h,
                                  D3D12_RESOURCE_STATES* st) {
  *w = s.model_output_width;
  *h = s.model_output_height;
  *st = s.model_output_state;
  return s.model_output;
}

ID3D12Resource* SelectComposedOutput(const DebugSnapshot& s,
                                     unsigned int* w, unsigned int* h,
                                     D3D12_RESOURCE_STATES* st) {
  *w = s.composed_output_width;
  *h = s.composed_output_height;
  *st = s.composed_output_state;
  return s.composed_output;
}

ID3D12Resource* SelectMotion(const DebugSnapshot& s,
                             unsigned int* w, unsigned int* h,
                             D3D12_RESOURCE_STATES* st) {
  *w = s.motion_width;
  *h = s.motion_height;
  *st = s.motion_state;
  return s.motion;
}

ID3D12Resource* SelectDepth(const DebugSnapshot& s,
                            unsigned int* w, unsigned int* h,
                            D3D12_RESOURCE_STATES* st) {
  *w = s.depth_width;
  *h = s.depth_height;
  *st = s.depth_state;
  return s.depth;
}

}  // namespace

const DebugViewDef kDebugViews[] = {
    {"Model Input (source)",    DebugDisplayMode::Color,  SelectSource},
    {"Model Input (prepared)",  DebugDisplayMode::Color,  SelectPreparedInput},
    {"Model Output (raw)",      DebugDisplayMode::Color,  SelectModelOutput},
    {"Model Output (composed)", DebugDisplayMode::Color,  SelectComposedOutput},
    {"Motion Vectors",          DebugDisplayMode::Motion, SelectMotion},
    {"Depth",                   DebugDisplayMode::Depth,  SelectDepth},
    {nullptr,                   DebugDisplayMode::Color,  nullptr},
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
  D3D12_RESOURCE_STATES source_state =
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ID3D12Resource* source = kDebugViews[index].select(
      snapshot, &content_w, &content_h, &source_state);
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

  // The caller's tracker may not know the source's state (fresh context).
  // Seed it, then transition to SRV. Restore afterwards so the source's
  // tracked state stays truthful for whatever runs next.
  state.SetResourceState(source, source_state);
  state.Transition(command_list, source,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  state.Transition(command_list, target,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  ID3D12Resource* srvs[kSrvCount] = {source, source, source, source};
  ID3D12Resource* uavs[kUavCount] = {target};

  const bool ok =
      codec.Dispatch("DebugDisplay", command_list, constants, srvs, uavs);

  state.Transition(command_list, source, source_state);
  return ok;
}

}  // namespace reshade_dlssnr

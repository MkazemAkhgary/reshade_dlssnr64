#include "debug_view.h"

#include <wrl/client.h>

namespace reshade_dlssnr {

namespace {

// Scratch slot reserved for debug rendering. Slot 0 is used by the
// Post-DLSS multi-pass snapshot; we deliberately keep them separate.
constexpr uint32_t kDebugScratchSlot = 1;

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
    {"Model Input (source)",    DebugDisplayMode::Color,  SelectSource},
    {"Model Input (prepared)",  DebugDisplayMode::Color,  SelectPreparedInput},
    {"Model Output (raw)",      DebugDisplayMode::Color,  SelectModelOutput},
    {"Model Output (composed)", DebugDisplayMode::Color,  SelectComposedOutput},
    {"Motion Vectors",          DebugDisplayMode::Motion, SelectMotion},
    {"Depth",                   DebugDisplayMode::Depth,  SelectDepth},
};

const uint32_t kDebugViewCount =
    sizeof(kDebugViews) / sizeof(kDebugViews[0]);

ID3D12Resource* DisplayDebugView(
    CommandContext& state,
    ComputeEngine& codec,
    ID3D12GraphicsCommandList* command_list,
    PassResources& resources,
    const DebugSnapshot& snapshot,
    int debug_view,
    unsigned int target_width,
    unsigned int target_height) {
  if (debug_view <= 0 || command_list == nullptr ||
      target_width == 0 || target_height == 0) {
    return nullptr;
  }

  const uint32_t index = static_cast<uint32_t>(debug_view) - 1;
  if (index >= kDebugViewCount) return nullptr;

  unsigned int content_w = 0;
  unsigned int content_h = 0;
  ID3D12Resource* source =
      kDebugViews[index].select(snapshot, &content_w, &content_h);
  if (source == nullptr || content_w == 0 || content_h == 0) {
    return nullptr;
  }

  Microsoft::WRL::ComPtr<ID3D12Device> device;
  if (FAILED(command_list->GetDevice(IID_PPV_ARGS(&device))) ||
      device == nullptr) {
    return nullptr;
  }

  // Match the composite output's format so the caller's CopyResource to the
  // backbuffer (or to the DLSS output) is always format-compatible.
  ID3D12Resource* composite = resources.GetCompositeOutput();
  if (composite == nullptr) return nullptr;

  D3D12_RESOURCE_DESC target_desc = composite->GetDesc();
  target_desc.Width = target_width;
  target_desc.Height = target_height;
  target_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (!resources.EnsureScratch(device.Get(), target_desc, target_width,
                               target_height, kDebugScratchSlot)) {
    return nullptr;
  }

  ID3D12Resource* target = resources.GetScratch(kDebugScratchSlot);
  if (target == nullptr) return nullptr;

  const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
  const D3D12_RESOURCE_DESC actual_target_desc = target->GetDesc();

  ComputeConstants constants = {};
  constants.Width = static_cast<unsigned int>(actual_target_desc.Width);
  constants.Height = static_cast<unsigned int>(actual_target_desc.Height);
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

  ID3D12Resource* srvs[kSrvCount] = {source, source, source, source};
  ID3D12Resource* uavs[kUavCount] = {target};

  if (!codec.Dispatch("DebugDisplay", command_list, constants, srvs, uavs)) {
    return nullptr;
  }

  return target;
}

}  // namespace reshade_dlssnr

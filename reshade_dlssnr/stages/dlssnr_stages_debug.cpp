#include "dlssnr_stages_debug.h"

#include "dlssnr_stages_upscale.h"

namespace reshade_dlssnr::dlssnr_stages_debug {

bool Run(
    DlssNrCommandContext& command,
    DlssNrPassResources& resources,
    DlssNrCodec* codec,
    ID3D12CommandQueue* command_queue,
    ID3D12Resource* backbuffer,
    const DlssGuideState& guides,
    ID3D12Resource* model_input,
    unsigned int width,
    unsigned int height,
    int debug_view) {
  if (codec == nullptr || backbuffer == nullptr) return false;

  command.Reset();

  ID3D12Resource* debug_input = nullptr;
  unsigned int debug_guide_width = width;
  unsigned int debug_guide_height = height;

  if (debug_view == 1) {
    debug_input = model_input;
  } else if (debug_view == 2) {
    debug_input = guides.depth;
    debug_guide_width = guides.guide_width;
    debug_guide_height = guides.guide_height;
  } else if (debug_view == 3) {
    debug_input = guides.motion;
    debug_guide_width = guides.guide_width;
    debug_guide_height = guides.guide_height;
  } else if (debug_view == 5) {
    debug_input = guides.pre_ui_color;
  } else if (debug_view == 6) {
    if (guides.selected_pass_color != nullptr &&
        (guides.selected_width != width || guides.selected_height != height)) {
      if (!dlssnr_stages_upscale::Apply(
              command,
              resources,
              codec,
              nullptr,
              guides.selected_pass_color,
              backbuffer,
              guides.selected_width,
              guides.selected_height,
              width,
              height)) {
        return false;
      }

      command.GetCommandList()->Close();
      ID3D12CommandList* lists[] = {command.GetCommandList()};
      command_queue->ExecuteCommandLists(1, lists);
      command.WaitGpu(command_queue);
      return true;
    }

    debug_input = guides.selected_pass_color;
    debug_guide_width = guides.selected_width != 0 ? guides.selected_width : width;
    debug_guide_height = guides.selected_height != 0 ? guides.selected_height : height;
  }

  if (debug_input == nullptr) return false;

  DlssNrConstants constants = {};
  constants.Mode = DlssNrMode_Resolve;
  constants.DebugView = 1;
  constants.Passthrough = 1;
  constants.WhitePoint = 1.0f;
  constants.Width = width;
  constants.Height = height;
  constants.GuideWidth = debug_guide_width;
  constants.GuideHeight = debug_guide_height;
  constants.MvScaleX = 1.0f;
  constants.MvScaleY = 1.0f;

  codec->Dispatch(
      command.GetCommandList(),
      constants,
      debug_input,
      debug_input,
      debug_input,
      nullptr,
      nullptr,
      resources.output,
      nullptr);

  command.Barrier(command.GetCommandList(), resources.output,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_COPY_SOURCE);
  command.Barrier(command.GetCommandList(), backbuffer,
                  D3D12_RESOURCE_STATE_PRESENT,
                  D3D12_RESOURCE_STATE_COPY_DEST);
  command.GetCommandList()->CopyResource(backbuffer, resources.output);
  command.Barrier(command.GetCommandList(), backbuffer,
                  D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PRESENT);
  command.Barrier(command.GetCommandList(), resources.output,
                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  command.GetCommandList()->Close();
  ID3D12CommandList* lists[] = {command.GetCommandList()};
  command_queue->ExecuteCommandLists(1, lists);
  command.WaitGpu(command_queue);

  return true;
}

}  // namespace reshade_dlssnr::dlssnr_stages_debug

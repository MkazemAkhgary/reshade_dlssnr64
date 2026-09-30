#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include "command_context.h"
#include "compute_engine.h"
#include "feature_lifecycle.h"
#include "state/settings.h"

namespace reshade_dlssnr {

class HdrState;
class NgxRuntime;
struct DlssGuideState;

// Runs the model at reduced scale with tone and structure edits suppressed
// and skin edits on, then thresholds the difference against its input to
// produce a per-pixel skin mask. The mask gates the HDR histogram's skin
// slice.
//
// DLSS Output only. Records on the game's command list.
class SkinDetect {
 public:
  struct Output {
    ID3D12Resource* mask = nullptr;          // R8_UNORM
    ID3D12Resource* detect_input = nullptr;  // SDR working input
    ID3D12Resource* detect_output = nullptr; // model output, SDR
    unsigned int width = 0;
    unsigned int height = 0;
    bool valid = false;
  };

  void Shutdown(NgxRuntime& runtime);

  bool Run(CommandContext& cmd,
           NgxRuntime& runtime,
           ComputeEngine& codec,
           ID3D12GraphicsCommandList* command_list,
           const DlssGuideState& guides,
           HdrState* hdr,
           const Settings& settings);

  const Output& GetOutput() const { return output_; }

 private:
  bool EnsureResources(ID3D12Device* device,
                       unsigned int width,
                       unsigned int height,
                       DXGI_FORMAT format);

  FeatureLifecycle feature_;
  Microsoft::WRL::ComPtr<ID3D12Resource> working_input_;
  Microsoft::WRL::ComPtr<ID3D12Resource> working_output_;
  // Ping-pong: each frame reads the previous frame's mask and writes the
  // current one, so the shader can blend the new vote against it.
  Microsoft::WRL::ComPtr<ID3D12Resource> mask_[2];
  uint32_t mask_index_ = 0;
  bool mask_valid_ = false;
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

  PassSettings tuning_;
  Output output_;
};

}  // namespace reshade_dlssnr
